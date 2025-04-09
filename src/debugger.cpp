#include "debugger.h"
#include "utlbuffer.h"
#include <assert.h>
#include <ctype.h>
#include <deque>
#include <filesystem>
#include <fmt/printf.h>
#include <fstream>
#include <mutex>
#include <setjmp.h>
#include <signal.h>
#include <sstream>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <thread>
#include <time.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifndef DEBUG
#define DEBUG 1
#endif

#define ASIO_STANDALONE
#include <asio.hpp>

#include "sourcepawn/include/sp_vm_types.h"
#include <nlohmann/json.hpp>

using namespace sp;
using asio::ip::tcp;

// Classe para gerenciar uma sessão TCP
class TcpSession : public std::enable_shared_from_this<TcpSession>
{
public:
  static std::shared_ptr<TcpSession>
  create(tcp::socket socket)
  {
    return std::shared_ptr<TcpSession>(new TcpSession(std::move(socket)));
  }

  ~TcpSession() { close(); }

  void
  start()
  {
    fmt::print(stderr, "Starting TcpSession\n");
    fflush(stderr);

    // Configurações de socket para melhor detecção de desconexão
    asio::socket_base::keep_alive option(true);
    socket_.set_option(option);
    
    // Configura TCP no delay
    socket_.set_option(tcp::no_delay(true));

    // Configura o socket para keepalive mais agressivo
    #if defined(TCP_KEEPIDLE) && defined(TCP_KEEPINTVL) && defined(TCP_KEEPCNT)
      // Começa a enviar keepalive após 5 segundos de inatividade
      int keepalive_time = 5;
      // Envia keepalive a cada 1 segundo
      int keepalive_interval = 1;
      // Número máximo de tentativas antes de considerar a conexão perdida
      int keepalive_count = 3;

      setsockopt(socket_.native_handle(), IPPROTO_TCP, TCP_KEEPIDLE, &keepalive_time, sizeof(keepalive_time));
      setsockopt(socket_.native_handle(), IPPROTO_TCP, TCP_KEEPINTVL, &keepalive_interval, sizeof(keepalive_interval));
      setsockopt(socket_.native_handle(), IPPROTO_TCP, TCP_KEEPCNT, &keepalive_count, sizeof(keepalive_count));
    #endif

    // Inicia o timer de timeout com intervalo menor
    start_timeout_timer();

    // Inicia a leitura
    do_read();
  }

  void
  send(const char *data, size_t length)
  {
    if (!connected_)
    {
      fmt::print(stderr, "Attempted to send on disconnected socket\n");
      fflush(stderr);
      return;
    }

    auto self = shared_from_this();
    // Copia os dados para um buffer que será mantido vivo durante a operação
    // assíncrona
    auto buffer = std::make_shared<std::vector<char>>(data, data + length);

    asio::async_write(
        socket_, asio::buffer(*buffer),
        [this, self, buffer](std::error_code ec, std::size_t /*length*/)
        {
          if (ec)
          {
            handle_error(ec);
          }
        });
  }

  tcp::socket &
  socket()
  {
    return socket_;
  }
  bool
  is_connected() const
  {
    return connected_;
  }

  void
  set_data_callback(std::function<void(const char *, size_t)> cb)
  {
    data_callback_ = std::move(cb);
  }

  void
  set_disconnect_callback(std::function<void()> cb)
  {
    disconnect_callback_ = std::move(cb);
  }

private:
  TcpSession(tcp::socket socket)
      : socket_(std::move(socket)), connected_(true),
        last_activity_(std::chrono::steady_clock::now())
  {
    auto endpoint = socket_.remote_endpoint();
    fmt::print(stderr, "[CONNECT] Nova conexão de {}:{}\n", 
              endpoint.address().to_string(), endpoint.port());
    fflush(stderr);
  }

  void
  start_timeout_timer()
  {
    timer_ = std::make_shared<asio::steady_timer>(socket_.get_executor());
    check_timeout();
  }

  void
  check_timeout()
  {
    if (!connected_)
      return;

    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                       now - last_activity_)
                       .count();

    // Reduzir o timeout para 10 segundos
    if (elapsed > 10)
    { 
      fmt::print(stderr, "[TIMEOUT] Connection timed out after {} seconds of inactivity\n", elapsed);
      fflush(stderr);
      
      // Tentar enviar um ping antes de fechar
      try {
        // Envia um pacote de ping
        const char ping[] = {static_cast<char>(0xC0)}; // 0xC0 = comando de ping
        send(ping, sizeof(ping));
        
        // Atualiza o último momento de atividade
        last_activity_ = now;
      }
      catch (...) {
        // Se falhar ao enviar o ping, fecha a conexão
        fmt::print(stderr, "[TIMEOUT] Failed to send ping, closing connection\n");
        fflush(stderr);
        close();
        return;
      }
    }

    auto self = shared_from_this();
    // Verificar mais frequentemente (a cada 2 segundos)
    timer_->expires_after(std::chrono::seconds(2));
    timer_->async_wait([this, self](const std::error_code &ec)
                       {
      if (!ec && connected_)
        {
          check_timeout();
        } });
  }

  void
  do_read()
  {
    if (!connected_)
    {
      return;
    }

    auto self = shared_from_this();
    socket_.async_read_some(
        asio::buffer(data_, max_length),
        [this, self](std::error_code ec, std::size_t length)
        {
          if (!ec)
          {
            last_activity_ = std::chrono::steady_clock::now();
            
            // Log detalhado dos dados recebidos
            std::string hexDump = fmt::format("[READ] Recebido {} bytes | HexDump: ", length);
            for (size_t i = 0; i < length; ++i)
            {
              hexDump += fmt::format("{:02x} ", static_cast<unsigned char>(data_[i]));
            }
            fmt::print(stderr, "{}\n", hexDump);
            fflush(stderr);

            // Se for um pacote de 5 bytes, considerar como comando de desconexão
            if (length == 5)
            {
              fmt::print(stderr, "[DISCONNECT] Detectado pacote de desconexão (5 bytes)\n");
              fflush(stderr);
              
              // Envia confirmação de desconexão antes de fechar
              const char response[] = {static_cast<char>(0xD1), 0x01}; // 0xD1 = CMD_DISCONNECT, 0x01 = sucesso
              send(response, sizeof(response));
              
              close();
              return;
            }

            if (data_callback_)
            {
              try
              {
                std::vector<char> dataCopy(data_, data_ + length);
                data_callback_(dataCopy.data(), length);

                if (connected_)
                {
                  do_read();
                }
              }
              catch (const std::exception &e)
              {
                fmt::print(stderr, "[ERROR] Erro no callback de dados: {}\n", e.what());
                fflush(stderr);
                handle_error(ec);
              }
            }
            else if (connected_)
            {
              do_read();
            }
          }
          else if (ec == asio::error::eof || 
                   ec == asio::error::connection_reset || 
                   ec == asio::error::operation_aborted ||
                   ec == asio::error::connection_aborted ||
                   ec == asio::error::broken_pipe)
          {
            fmt::print(stderr, "[DISCONNECT] Conexão encerrada pelo peer: {} ({})\n", 
                      ec.message(), ec.value());
            fflush(stderr);
            close();
          }
          else
          {
            fmt::print(stderr, "[ERROR] Erro de socket: {} ({})\n", 
                      ec.message(), ec.value());
            fflush(stderr);
            handle_error(ec);
          }
        });
  }

  void
  close()
  {
    if (std::exchange(connected_, false))
    { 
      auto endpoint = socket_.remote_endpoint();
      fmt::print(stderr, "[DISCONNECT] Fechando conexão com {}:{}\n", 
                endpoint.address().to_string(), endpoint.port());
      fflush(stderr);

      if (timer_)
      {
        error_code ec;
        timer_->cancel(ec);
      }

      if (socket_.is_open())
      {
        error_code ec;
        socket_.shutdown(tcp::socket::shutdown_both, ec);
        if (ec && ec != asio::error::not_connected)
        {
          fmt::print(stderr, "[ERROR] Erro no shutdown do socket: {}\n", ec.message());
          fflush(stderr);
        }

        socket_.close(ec);
        if (ec)
        {
          fmt::print(stderr, "[ERROR] Erro fechando socket: {}\n", ec.message());
          fflush(stderr);
        }
      }

      if (disconnect_callback_)
      {
        try
        {
          fmt::print(stderr, "[DISCONNECT] Executando callback de desconexão\n");
          fflush(stderr);
          disconnect_callback_();
        }
        catch (const std::exception &e)
        {
          fmt::print(stderr, "[ERROR] Erro no callback de desconexão: {}\n", e.what());
          fflush(stderr);
        }
      }

      fmt::print(stderr, "[DISCONNECT] Conexão fechada completamente\n");
      fflush(stderr);
    }
  }

  void
  handle_error(const std::error_code &ec)
  {
    fmt::print(stderr, "Socket error: {}\n", ec.message());
    fflush(stderr);
    close();
  }

private:
  tcp::socket socket_;
  enum
  {
    max_length = 1024 * 1024
  };
  char data_[max_length];
  std::function<void(const char *, size_t)> data_callback_;
  std::function<void()> disconnect_callback_;
  bool connected_;
  std::chrono::steady_clock::time_point last_activity_;
  std::shared_ptr<asio::steady_timer> timer_;
  using error_code = asio::error_code;
};

// Substituir a definição antiga de TcpConnection::Ptr
using TcpConnectionPtr = std::shared_ptr<TcpSession>;

//
//  Lowercases string
//
template <typename T>
std::basic_string<T>
lowercase(const std::basic_string<T> &s)
{
  std::basic_string<T> s2 = s;
  std::transform(s2.begin(), s2.end(), s2.begin(), tolower);
  return std::move(s2);
}

enum DebugState
{
  DebugDead = -1,
  DebugRun = 0,
  DebugBreakpoint,
  DebugPause,
  DebugStepIn,
  DebugStepOver,
  DebugStepOut,
  DebugException
};
enum MessageType
{
  Diagnostics = 0,
  RequestFile,
  File,
  StartDebugging,
  StopDebugging,
  Pause,
  Continue,
  RequestCallStack,
  CallStack,
  ClearBreakpoints,
  SetBreakpoint,
  HasStopped,
  HasContinued,
  StepOver,
  StepIn,
  StepOut,
  RequestSetVariable,
  SetVariable,
  RequestVariables,
  Variables,
  RequestEvaluate,
  Evaluate,
  Disconnect,
  TotalMessages
};

std::vector<std::string>
split_string(const std::string &str, const std::string &delimiter)
{
  std::vector<std::string> strings;

  std::string::size_type pos = 0;
  std::string::size_type prev = 0;
  while ((pos = str.find(delimiter, prev)) != std::string::npos)
  {
    strings.push_back(str.substr(prev, pos - prev));
    prev = pos + 1;
  }

  // To get the last substring (or only, if delimiter is not found)
  strings.push_back(str.substr(prev));

  return strings;
}
DebugReport DebugListener;
void removeClientID(const TcpConnectionPtr &session);
class DebuggerClient
{
public:
  TcpConnectionPtr socket;
  std::unordered_set<std::string> files;
  int DebugState = 0;

  struct variable_s
  {
    std::string name;
    std::string value;
    std::string type;
  };

  struct call_stack_s
  {
    uint32_t line;
    std::string name;
    std::string filename;
  };

  struct breakpoint_s
  {
    long line;
    std::string filename;
  };

public:
  bool unload = false;
  bool receive_walk_cmd = false;
  std::mutex mtx;
  std::condition_variable cv;
  SourcePawn::IPluginContext *context_;
  uint32_t current_line;
  std::unordered_map<std::string, std::unordered_set<long>> break_list;
  int current_state = 0;
  cell_t lastfrm_ = 0;
  cell_t cip_;
  cell_t frm_;
  std::map<std::string, std::shared_ptr<SmxV1Image>> images;
  std::shared_ptr<SmxV1Image> current_image = nullptr;
  SourcePawn::IFrameIterator *debug_iter;
  DebuggerClient(const TcpConnectionPtr &tcp_connection)
      : socket(tcp_connection)
  {
  }

  ~DebuggerClient()
  {
    stopDebugging();
    fmt::print("Debugger disabled.\n");
  }

  class debugger_stopped : public std::exception
  {
    std::string what_message;

  public:
    const char *
    what() const throw()
    {
      return "Debugger exited!";
    }
  };

  void
  setBreakpoint(std::string path, int line, int id)
  {
    break_list[path].insert(line);
  }

  void
  clearBreakpoints(std::string fileName)
  {
    auto found = break_list.find(fileName);
    if (found != break_list.end())
    {
      found->second.clear();
    }
  }

  enum
  {
    DISP_DEFAULT = 0x10,
    DISP_STRING = 0x20,
    DISP_BIN = 0x30, /* ??? not implemented */
    DISP_HEX = 0x40,
    DISP_BOOL = 0x50,
    DISP_FIXED = 0x60,
    DISP_FLOAT = 0x70
  };
#define MAX_DIMS 3
#define DISP_MASK 0x0f

  char *
  get_string(SmxV1Image::Symbol *sym)
  {
    assert(sym->ident() == sp::IDENT_ARRAY || sym->ident() == sp::IDENT_REFARRAY);
    assert(sym->dimcount() == 1);

    // get the starting address and the length of the string
    cell_t *addr;
    cell_t base = sym->addr();
    if (sym->vclass() == 1 || sym->vclass() == 3) // local var or arg but not static
      base += frm_;                               // addresses of local vars are relative to the frame
    if (sym->ident() == sp::IDENT_REFARRAY)
    {
      context_->LocalToPhysAddr(base, &addr);
      assert(addr != nullptr);
      base = *addr;
    }

    char *str;
    if (context_->LocalToStringNULL(base, &str) != SP_ERROR_NONE)
      return nullptr;
    return str;
  }

  int get_symbolvalue(const SmxV1Image::Symbol *sym, int index, cell_t *value)
  {
    cell_t *vptr;
    cell_t base = sym->addr();
    if (sym->vclass() & DISP_MASK)
      base += frm_; // addresses of local vars are relative to the frame

    // a reference
    if (sym->ident() == sp::IDENT_REFERENCE || sym->ident() == sp::IDENT_REFARRAY)
    {
      if (context_->LocalToPhysAddr(base, &vptr) != SP_ERROR_NONE)
        return false;

      assert(vptr != nullptr);
      base = *vptr;
    }

    if (context_->LocalToPhysAddr(base + index * sizeof(cell_t), &vptr) != SP_ERROR_NONE)
      return false;

    if (vptr != nullptr)
      *value = *vptr;
    return vptr != nullptr;
  }

  void
  printvalue(long value, int disptype, std::string &out_value,
             std::string &out_type)
  {
    char out[64];
    if (disptype == DISP_FLOAT)
    {
      out_type = "float";
      sprintf(out, "%f", sp_ctof(value));
    }
    else if (disptype == DISP_FIXED)
    {
      out_type = "fixed";
#define MULTIPLIER 1000
      long ipart = value / MULTIPLIER;
      value -= MULTIPLIER * ipart;
      if (value < 0)
        value = -value;
      sprintf(out, "%ld.%03ld", ipart, value);
    }
    else if (disptype == DISP_HEX)
    {
      out_type = "hex";
      sprintf(out, "%lx", value);
    }
    else if (disptype == DISP_BOOL)
    {
      out_type = "bool";
      switch (value)
      {
      case 0:
        sprintf(out, "false");
        break;
      case 1:
        sprintf(out, "true");
        break;
      default:
        sprintf(out, "%ld (true)", value);
        break;
      } /* switch */
    }
    else
    {
      out_type = "cell";
      sprintf(out, "%ld", value);
    } /* if */
    out_value += out;
  }
  nlohmann::json
  read_variable(uint32_t &addr, uint32_t type_id, debug::Rtti *rtti,
                bool is_ref = false)
  {
    nlohmann::json json;
    if (!rtti)
    {
      rtti = const_cast<debug::Rtti *>(
          current_image->rtti_data()->typeFromTypeId(type_id));
    }
    cell_t *ptr;
    switch (rtti->type())
    {
    case cb::kAny:
    {
      context_->LocalToPhysAddr(addr, &ptr);
      json = (int32_t)*ptr;
    }
    case cb::kBool:
    {
      context_->LocalToPhysAddr(addr, &ptr);
      json = (bool)*ptr;
      break;
    }
    case cb::kInt32:
    {
      context_->LocalToPhysAddr(addr, &ptr);
      json = (int32_t)*ptr;
      break;
    }
    case cb::kFloat32:
    {
      context_->LocalToPhysAddr(addr, &ptr);
      json = sp_ctof(*ptr);
      break;
    }
    case cb::kFixedArray:
    {
      if (rtti->inner())
      {
        if (rtti->inner()->type() == cb::kChar8)
        {
          json = read_variable(
              addr, rtti->inner()->type(),
              const_cast<debug::Rtti *>(rtti->inner()), false);
        }
        else
        {
          for (int i = 0; i < rtti->index(); i++)
          {
            uint32_t start = addr;

            json[i] = read_variable(
                start, rtti->inner()->type(),
                const_cast<debug::Rtti *>(rtti->inner()), false);
            addr += 4;
          }
        }
      }
      break;
    }
    case cb::kChar8:
    {
      char *str = nullptr;
      if (context_->LocalToStringNULL(addr, &str) != SP_ERROR_NONE)
      {
        break;
      }
      if (str)
      {
        addr += strlen(str) + 1;
      }
      if (addr % sizeof(cell_t) != 0)
      {
        addr += sizeof(cell_t) - (addr % sizeof(cell_t));
      }
      json = str ? str : "";
      break;
    }
    case cb::kArray:
    {
      if (is_ref)
      {
        cell_t *a;
        context_->LocalToPhysAddr(addr, &a);
        addr = *a;
      }
      if (rtti->inner())
      {
        json = read_variable(addr, rtti->inner()->type(),
                             const_cast<debug::Rtti *>(rtti->inner()));
      }
      break;
    }
    case cb::kEnumStruct:
    {
      auto fields = current_image->getEnumFields(rtti->index());

      uint32_t start = addr;

      for (auto &field : fields)
      {
        auto name = current_image->GetDebugName(field->name);
        auto rtti_field = current_image->rtti_data()->typeFromTypeId(
            field->type_id);
        if (!rtti_field)
        {
          break;
        }
        json[name] = read_variable(start, rtti_field->type(),
                                   (sp::debug::Rtti *)rtti_field);
      }
      break;
    }
    case cb::kClassdef:
    {
      auto fields = current_image->getTypeFields(rtti->index());
      cell_t *ptr;
      uint32_t field_offset = addr;

      for (auto &field : fields)
      {
        uint32_t start = field_offset;

        auto name = current_image->GetDebugName(field->name);
        auto rtti_field = current_image->rtti_data()->typeFromTypeId(
            field->type_id);
        json[name] = read_variable(start, rtti_field->type(),
                                   (sp::debug::Rtti *)rtti_field, true);
        field_offset += sizeof(cell_t);
      }
      break;
    }
    }

    return json;
  }
  variable_s
  display_variable(SmxV1Image::Symbol *sym, uint32_t index[], int idxlevel,
                   bool noarray = false)
  {
    nlohmann::json json;
    variable_s var;
    var.name = "N/A";
    if (current_image->GetDebugName(sym->name()) != nullptr)
    {
      var.name = current_image->GetDebugName(sym->name());
    };
    var.type = "N/A";
    var.value = "";
    cell_t value;
    std::unique_ptr<std::vector<SmxV1Image::ArrayDim *>> symdims;
    assert(index != NULL);
    auto rtti = sym->rtti();
    if (rtti && rtti->type_id)
    {
      uint32_t base = static_cast<uint32_t>(rtti->address);
      if (sym->vclass() == 1 || sym->vclass() == 3) // local var or arg but not static
        base += frm_;                               // addresses of local vars are relative to the frame

      try
      {
        auto json = read_variable(base, rtti->type_id, nullptr,
                                  sym->vclass() == 0x3);
        if (!json.empty())
        {
          var.value = json.dump();
          return var;
        }
      }
      catch (...)
      {
        // skip rtti parse
      }
    }
    // first check whether the variable is visible at all
    if ((uint32_t)cip_ < sym->codestart() || (uint32_t)cip_ > sym->codeend())
    {
      var.value = "Not in scope.";
      return var;
    }

    // set default display type for the symbol (if none was set)
    if ((sym->vclass() & ~DISP_MASK) == 0)
    {
      const char *tagname = current_image->GetTagName(sym->tagid());
      if (tagname != nullptr)
      {
        if (!strcasecmp(tagname, "bool"))
        {
          sym->setVClass(sym->vclass() | DISP_BOOL);
        }
        else if (!stricmp(tagname, "float"))
        {
          sym->setVClass(sym->vclass() | DISP_FLOAT);
        }
      }
      if ((sym->vclass() & ~DISP_MASK) == 0 && (sym->ident() == sp::IDENT_ARRAY || sym->ident() == sp::IDENT_REFARRAY) && sym->dimcount() == 1)
      {
        /* untagged array with a single dimension, walk through all
         * elements and check whether this could be a string
         */
        char *ptr = get_string(sym);
        if (ptr != nullptr)
        {
          uint32_t i;
          for (i = 0; ptr[i] != '\0'; i++)
          {
            if ((ptr[i] < ' ' && ptr[i] != '\n' && ptr[i] != '\r' && ptr[i] != '\t'))
              break; // non-ASCII character
            if (i == 0 && !isalpha(ptr[i]))
              break; // want a letter at the start
          }
          if (i > 0 && ptr[i] == '\0')
            sym->setVClass(sym->vclass() | DISP_STRING);
        }
      }
    }

    if (sym->ident() == sp::IDENT_ARRAY || sym->ident() == sp::IDENT_REFARRAY)
    {
      int dim;
      symdims = std::make_unique<std::vector<SmxV1Image::ArrayDim *>>(
          *current_image->GetArrayDimensions(sym));
      // check whether any of the indices are out of range
      assert(symdims != nullptr);
      for (dim = 0; dim < idxlevel; dim++)
      {
        if (symdims->at(dim)->size() > 0 && index[dim] >= symdims->at(dim)->size())
          break;
      }
      if (dim < idxlevel)
      {
        var.value = "(index out of range)";
        return var;
      }
    }

    // Print first dimension of array
    if ((sym->ident() == sp::IDENT_ARRAY || sym->ident() == sp::IDENT_REFARRAY) && idxlevel == 0)
    {
      // Print string
      if ((sym->vclass() & ~DISP_MASK) == DISP_STRING)
      {
        var.type = "String";
        char *str = get_string(sym);
        if (str != nullptr)
        {
          var.value = str;
        }
        else
          var.value = "NULL_STRING";
      }
      // Print one-dimensional array
      else if (sym->dimcount() == 1)
      {
        if (!noarray)
          var.type = "Array";
        assert(symdims != nullptr); // set in the previous block
        uint32_t len = symdims->at(0)->size();
        uint32_t i;
        auto type = (sym->vclass() & ~DISP_MASK);
        if (type == DISP_FLOAT)
        {
          json = std::vector<float>();
        }
        else if (type == DISP_BOOL)
        {
          json = std::vector<bool>();
        }
        else
        {
          json = std::vector<cell_t>();
        }
        for (i = 0; i < len; i++)
        {
          if (get_symbolvalue(sym, i, &value))
          {
            if (type == DISP_FLOAT)
            {
              json.push_back(sp_ctof(value));
            }
            else if (type == DISP_BOOL)
            {
              json.push_back(value);
            }
            else
            {
              json.push_back(value);
            }
          }
        }
        var.value = json.dump(4).c_str();
      }
      // Not supported..
      else
      {
        var.value = "(multi-dimensional array)";
      }
    }
    else if (sym->ident() != sp::IDENT_ARRAY && sym->ident() != sp::IDENT_REFARRAY && idxlevel > 0)
    {
      // index used on a non-array
      var.value = "(invalid index, not an array)";
    }
    else
    {
      // simple variable, or indexed array element
      assert(idxlevel > 0 || index[0] == 0); // index should be zero if non-array
      int dim;
      int base = 0;
      for (dim = 0; dim < idxlevel - 1; dim++)
      {
        if (!noarray)
          var.type = "Array";
        base += index[dim];
        if (!get_symbolvalue(sym, base, &value))
          break;
        base += value / sizeof(cell_t);
      }

      if (get_symbolvalue(sym, base + index[dim], &value) && sym->dimcount() == idxlevel)
        printvalue(value, (sym->vclass() & ~DISP_MASK), var.value,
                   var.type);
      else if (sym->dimcount() != idxlevel)
        var.value = "(invalid number of dimensions)";
      else
        var.value = "(?)";
    }
    return var;
  }

  void
  evaluateVar(int frame_id, char *variable)
  {
    if (current_state != DebugRun)
    {
      auto imagev1 = current_image.get();

      std::unique_ptr<SmxV1Image::Symbol> sym;
      if (imagev1->GetVariable(variable, cip_, sym))
      {
        uint32_t idx[MAX_DIMS], dim;
        dim = 0;
        memset(idx, 0, sizeof idx);
        auto var = display_variable(sym.get(), idx, dim);
        CUtlBuffer buffer;
        buffer.PutUnsignedInt(0);
        {
          buffer.PutChar(MessageType::Evaluate);
          buffer.PutInt(var.name.size() + 1);
          buffer.PutString(var.name.c_str());
          buffer.PutInt(var.value.size() + 1);
          buffer.PutString(var.value.c_str());
          ;
          buffer.PutInt(var.type.size() + 1);
          buffer.PutString(var.type.c_str());
          buffer.PutInt(0);
        }
        *(uint32_t *)buffer.Base() = buffer.TellPut() - 5;
        socket->send(static_cast<const char *>(buffer.Base()),
                     static_cast<size_t>(buffer.TellPut()));
      }
    }
  }

  int set_symbolvalue(const SmxV1Image::Symbol *sym, int index, cell_t value)
  {
    cell_t *vptr;
    cell_t base = sym->addr();
    if (sym->vclass() & DISP_MASK)
      base += frm_; // addresses of local vars are relative to the frame

    // a reference
    if (sym->ident() == sp::IDENT_REFERENCE || sym->ident() == sp::IDENT_REFARRAY)
    {
      context_->LocalToPhysAddr(base, &vptr);
      assert(vptr != nullptr);
      base = *vptr;
    }

    context_->LocalToPhysAddr(base + index * sizeof(cell_t), &vptr);
    assert(vptr != nullptr);
    *vptr = value;
    return true;
  }

  bool
  SetSymbolString(const SmxV1Image::Symbol *sym, char *str)
  {
    assert(sym->ident() == sp::IDENT_ARRAY || sym->ident() == sp::IDENT_REFARRAY);
    assert(sym->dimcount() == 1);

    cell_t *vptr;
    cell_t base = sym->addr();
    if (sym->vclass() & DISP_MASK)
      base += frm_; // addresses of local vars are relative to the frame

    // a reference
    if (sym->ident() == sp::IDENT_REFERENCE || sym->ident() == sp::IDENT_REFARRAY)
    {
      context_->LocalToPhysAddr(base, &vptr);
      assert(vptr != nullptr);
      base = *vptr;
    }

    std::unique_ptr<std::vector<SmxV1Image::ArrayDim *>> dims;
    dims = std::make_unique<std::vector<SmxV1Image::ArrayDim *>>(
        *current_image->GetArrayDimensions(sym));
    return context_->StringToLocalUTF8(base, dims->at(0)->size(), str, NULL) == SP_ERROR_NONE;
  }

  void
  setVariable(std::string var, std::string value, int index)
  {
    bool success = false;
    bool valid_value = true;
    if (current_state != DebugRun)
    {
      auto imagev1 = current_image.get();
      std::unique_ptr<SmxV1Image::Symbol> sym;
      cell_t result = 0;
      value.erase(remove(value.begin(), value.end(), '\"'),
                  value.end());
      if (imagev1->GetVariable(var.c_str(), cip_, sym))
      {
        if ((sym->ident() == IDENT_ARRAY || sym->ident() == IDENT_REFARRAY))
        {
          if ((sym->vclass() & ~DISP_MASK) == DISP_STRING)
          {
            SetSymbolString(sym.get(),
                            const_cast<char *>(value.c_str()));
          }
          valid_value = false;
        }
        else
        {
          size_t lastChar;
          try
          {
            int intvalue = std::stoi(value, &lastChar);
            if (lastChar == value.size())
            {
              result = intvalue;
            }
            else
            {
              auto val = std::stof(value, &lastChar);
              result = sp_ftoc(val);
            }
          }
          catch (...)
          {
            // ??? some text or bool
            if (value == "true")
            {
              result = 1;
            }
            else if (value == "false")
            {
              result = 0;
            }
            else
            {
              valid_value = false;
            }
          }
        }

        if (valid_value && (imagev1->GetVariable(var.c_str(), cip_, sym)))
        {
          success = set_symbolvalue(sym.get(), index, (cell_t)result);
        }
      }
    }
    CUtlBuffer buffer;
    buffer.PutUnsignedInt(0);
    {
      buffer.PutChar(MessageType::SetVariable);
      buffer.PutInt(success);
    }
    *(uint32_t *)buffer.Base() = buffer.TellPut() - 5;
    socket->send(static_cast<const char *>(buffer.Base()),
                 static_cast<size_t>(buffer.TellPut()));
  }

  void
  sendVariables(char *scope)
  {
    bool local_scope = strstr(scope, ":%local%");
    bool global_scope = strstr(scope, ":%global%");
    if (current_state != DebugRun)
    {
      auto imagev1 = current_image.get();

      std::unique_ptr<SmxV1Image::Symbol> sym;
      if (current_image && imagev1)
      {
#define sDIMEN_MAX 4
        uint32_t idx[sDIMEN_MAX], dim;
        dim = 0;
        memset(idx, 0, sizeof idx);
        std::vector<variable_s> vars;
        if (local_scope || global_scope)
        {
          SmxV1Image::SymbolIterator iter = imagev1->symboliterator(global_scope);
          while (!iter.Done())
          {
            const auto sym = iter.Next();

            // Only variables in scope.
            if (sym->ident() != sp::IDENT_FUNCTION && (sym->codestart() <= (uint32_t)cip_ && sym->codeend() >= (uint32_t)cip_) || global_scope)
            {
              auto var = display_variable(sym, idx, dim);
              if (local_scope)
              {
                if ((sym->vclass() & DISP_MASK) > 0)
                {
                  vars.push_back(var);
                }
              }
              else
              {
                if (!((sym->vclass() & DISP_MASK) > 0))
                {
                  vars.push_back(var);
                }
              }
            }
          }
        }
        else
        {
          if (imagev1->GetVariable(scope, cip_, sym))
          {
            auto var = display_variable(sym.get(), idx, dim, true);
            std::string var_name = scope;
            auto values = split_string(var.value, ",");
            int i = 0;
            for (auto val : values)
            {
              vars.push_back({std::to_string(i), val, var.type});
              i++;
            }
          }
        }
        CUtlBuffer buffer;
        buffer.PutUnsignedInt(0);
        buffer.PutChar(Variables);
        buffer.PutInt(strlen(scope) + 1);
        buffer.PutString(scope);
        buffer.PutInt(vars.size());
        for (auto var : vars)
        {
          buffer.PutInt(var.name.size() + 1);
          buffer.PutString(var.name.c_str());
          buffer.PutInt(var.value.size() + 1);
          buffer.PutString(var.value.c_str());
          ;
          buffer.PutInt(var.type.size() + 1);
          buffer.PutString(var.type.c_str());
          buffer.PutInt(0);
        }
        *(uint32_t *)buffer.Base() = buffer.TellPut() - 5;
        socket->send(static_cast<const char *>(buffer.Base()),
                     static_cast<size_t>(buffer.TellPut()));
      }
    }
  }

  void
  CallStack()
  {
    std::vector<call_stack_s> callStack;
    if (current_state == DebugException)
    {
      if (debug_iter)
      {
        uint32_t index = 0;
        for (; !debug_iter->Done(); debug_iter->Next(), index++)
        {
          if (debug_iter->IsNativeFrame())
          {
            callStack.push_back(
                {0, debug_iter->FunctionName(), "native"});
          }
          else if (debug_iter->IsScriptedFrame())
          {
            auto current_file = std::filesystem::path(debug_iter->FilePath())
                                    .filename()
                                    .string();
            lowercase(current_file);
            callStack.push_back({debug_iter->LineNumber() - 1,
                                 debug_iter->FunctionName(),
                                 current_file});
          }
        }
      }
      current_state = DebugBreakpoint;
    }
    else if (current_state != DebugRun)
    {
      IFrameIterator *iter = context_->CreateFrameIterator();

      uint32_t index = 0;
      for (; !iter->Done(); iter->Next(), index++)
      {
        if (iter->IsNativeFrame())
        {
          callStack.push_back({0, iter->FunctionName(), ""});
        }
        else if (iter->IsScriptedFrame())
        {
          std::string current_file = iter->FilePath();
          for (auto file : files)
          {
            if (file.find(current_file) != std::string::npos)
            {
              current_file = file;
              break;
            }
          }
          callStack.push_back({iter->LineNumber() - 1,
                               iter->FunctionName(), current_file});
        }
      }
      context_->DestroyFrameIterator(iter);
    }

    CUtlBuffer buffer;
    buffer.PutUnsignedInt(0);
    {
      buffer.PutChar(MessageType::CallStack);
      buffer.PutInt(callStack.size());
      for (auto stack : callStack)
      {
        buffer.PutInt(stack.name.size() + 1);
        buffer.PutString(stack.name.c_str());
        buffer.PutInt(stack.filename.size() + 1);
        buffer.PutString(stack.filename.c_str());
        buffer.PutInt(stack.line + 1);
      }
    }
    *(uint32_t *)buffer.Base() = buffer.TellPut() - 5;
    socket->send(static_cast<const char *>(buffer.Base()),
                 static_cast<size_t>(buffer.TellPut()));
  }

  void
  WaitWalkCmd(std::string reason = "Breakpoint", std::string text = "N/A")
  {
    if (!receive_walk_cmd)
    {
      CUtlBuffer buffer;
      {
        buffer.PutUnsignedInt(0);
        {
          buffer.PutChar(MessageType::HasStopped);
          buffer.PutInt(reason.size() + 1);
          buffer.PutString(reason.c_str());
          buffer.PutInt(reason.size() + 1);
          buffer.PutString(reason.c_str());
          buffer.PutInt(text.size() + 1);
          buffer.PutString(text.c_str());
        }
        *(uint32_t *)buffer.Base() = buffer.TellPut() - 5;
      }
      socket->send(static_cast<const char *>(buffer.Base()),
                   static_cast<size_t>(buffer.TellPut()));
      std::unique_lock<std::mutex> lck(mtx);
      cv.wait(lck, [this]
              { return receive_walk_cmd; });
    }
    if (current_state == DebugDead)
    {
      unload = true;
      throw debugger_stopped();
    }
  }

  void
  ReportError(const IErrorReport &report, IFrameIterator &iter)
  {
    receive_walk_cmd = false;
    current_state = DebugException;
    context_ = iter.Context();
    debug_iter = &iter;
    WaitWalkCmd("exception", report.Message());
  }
  int(DebugHook)(SourcePawn::IPluginContext *ctx,
                 sp_debug_break_info_t &BreakInfo)
  {
    std::string filename = ctx->GetRuntime()->GetFilename();
    auto image = images.find(filename);
    if (image == images.end())
    {
      FILE *fp = fopen(filename.c_str(), "rb");
      current_image = std::make_shared<SmxV1Image>(fp);
      current_image->validate();
      images.insert({filename, current_image});
      fclose(fp);
    }
    else
    {
      current_image = image->second;
    }
    context_ = ctx;
    if (current_state == DebugDead)
      return current_state;

    context_ = ctx;
    cip_ = BreakInfo.cip;
    // Reset the state.
    frm_ = BreakInfo.frm;
    receive_walk_cmd = false;

    IFrameIterator *iter = context_->CreateFrameIterator();
    std::string current_file = "N/A";
    uint32_t index = 0;
    for (; !iter->Done(); iter->Next(), index++)
    {
      if (iter->IsNativeFrame())
      {
        continue;
      }

      if (iter->IsScriptedFrame())
      {
        current_file = std::filesystem::path(iter->FilePath())
                           .filename()
                           .string();
        lowercase(current_file);

        for (auto file : files)
        {
          if (file.find(current_file) != std::string::npos)
          {
            current_file = file;
            break;
          }
        }
        break;
      }
    }
    context_->DestroyFrameIterator(iter);

    static uint32_t lastline = 0;
    current_image->LookupLine(cip_, &current_line);
    // Reset the frame iterator, so stack traces start at the beginning
    // again.

    /* dont break twice */
    if (current_line == lastline)
      return current_state;

    lastline = current_line;
    if (current_state == DebugStepOut && frm_ > lastfrm_)
      current_state = DebugStepIn;

    if (current_state == DebugPause || current_state == DebugStepIn)
    {
      WaitWalkCmd();
    }
    else
    {
      auto found = break_list.find(current_file);
      if (found != break_list.end())
      {
        if (found->second.find(current_line) != found->second.end())
        {
          current_state = DebugBreakpoint;
          WaitWalkCmd();
        }
      }
    }

    /* check whether we are stepping through a sub-function */
    if (current_state == DebugStepOver)
    {
      if (frm_ < lastfrm_)
      {
        return current_state;
      }

      WaitWalkCmd();

      if (current_state == DebugDead)
        return current_state;
    }

    lastfrm_ = frm_;

    return current_state;
  }

  void
  SwitchState(unsigned char state)
  {
    current_state = state;
    receive_walk_cmd = true;
    cv.notify_one();
  }

  void
  AskFile()
  {
  }

  void
  RecvDebugFile(CUtlBuffer *buf)
  {
    char file[260];
    int strlen = buf->GetInt();
    buf->GetString(file, strlen);
    auto filename = std::filesystem::path(file).filename().string();
    lowercase(filename);
    files.insert(filename);
  }

  void
  RecvStateSwitch(CUtlBuffer *buf)
  {
    auto CurrentState = buf->GetUnsignedChar();
    SwitchState(CurrentState);
  }

  void
  RecvCallStack(CUtlBuffer *buf)
  {
    CallStack();
  }

  void
  recvRequestVariables(CUtlBuffer *buf)
  {
    char scope[256];
    int strlen = buf->GetInt();
    buf->GetString(scope, strlen);
    sendVariables(scope);
  }

  void
  recvRequestEvaluate(CUtlBuffer *buf)
  {
    int frameId;
    char variable[256];
    int strlen = buf->GetInt();
    buf->GetString(variable, strlen);
    frameId = buf->GetInt();
    evaluateVar(frameId, variable);
  }

  void
  recvDisconnect(CUtlBuffer *buf)
  {
    removeClientID(socket);
  }

  void
  recvBreakpoint(CUtlBuffer *buf)
  {
    char path[256];
    int strlen = buf->GetInt();
    buf->GetString(path, strlen);
    std::string filename(std::filesystem::path(path).filename().string());
    lowercase(filename);
    files.insert(filename);
    int line = buf->GetInt();
    int id = buf->GetInt();
    setBreakpoint(filename, line, id);
  }

  void
  recvClearBreakpoints(CUtlBuffer *buf)
  {
    char path[256];
    int strlen = buf->GetInt();
    buf->GetString(path, strlen);

    std::string filename(std::filesystem::path(path).filename().string());
    lowercase(filename);
    clearBreakpoints(filename);
  }

  void
  stopDebugging()
  {
    current_state = DebugDead;
    receive_walk_cmd = true;
    cv.notify_one();
    std::unique_lock<std::mutex> lck(mtx);
    cv.wait(lck, [this]
            { return unload; });
  }

  void
  recvStopDebugging(CUtlBuffer *buf)
  {
    stopDebugging();
    removeClientID(socket);
  }

  void
  recvRequestSetVariable(CUtlBuffer *buf)
  {
    char var[256];
    int strlen = buf->GetInt();
    buf->GetString(var, strlen);
    char value[256];
    strlen = buf->GetInt();
    buf->GetString(value, strlen);
    auto index = buf->GetInt();
    setVariable(var, value, index);
  }

  void
  RecvCmd(const char *buffer, size_t len)
  {
    CUtlBuffer buf((void *)buffer, len);
    while (buf.TellGet() < len)
    {
      int msg_len = buf.GetUnsignedInt();
      int type = buf.GetUnsignedChar();
      switch (type)
      {
      case RequestFile:
      {
        RecvDebugFile(&buf);
        break;
      }
      case Pause:
      {
        RecvStateSwitch(&buf);
        break;
      }
      case Continue:
      {
        RecvStateSwitch(&buf);
        break;
      }
      case StepIn:
      {
        RecvStateSwitch(&buf);
        break;
      }
      case StepOver:
      {
        RecvStateSwitch(&buf);
        break;
      }
      case StepOut:
      {
        RecvStateSwitch(&buf);
        break;
      }
      case RequestCallStack:
      {
        RecvCallStack(&buf);
        break;
      }
      case RequestVariables:
      {
        recvRequestVariables(&buf);
        break;
      }
      case RequestEvaluate:
      {
        recvRequestEvaluate(&buf);
        break;
      }
      case Disconnect:
      {
        recvDisconnect(&buf);
        break;
      }
      case ClearBreakpoints:
      {
        recvClearBreakpoints(&buf);
        break;
      }
      case SetBreakpoint:
      {
        recvBreakpoint(&buf);
        break;
      }
      case StopDebugging:
      {
        recvStopDebugging(&buf);
        break;
      }
      case RequestSetVariable:
      {
        recvRequestSetVariable(&buf);
        break;
      }
      }
    }
  }
};

std::vector<std::unique_ptr<DebuggerClient>> clients;

void addClientID(const TcpConnectionPtr &session)
{
  // Adicionar logging para debug
  fmt::print(stderr, "Attempting to add client ID...\n");
  fflush(stderr);

  try
  {
    if (auto it = std::find_if(clients.begin(), clients.end(),
                               [&session](const auto &client)
                               {
                                 return client->socket == session;
                               });
        it == clients.end())
    {
      fmt::print(stderr, "Client not found, adding new client...\n");
      fflush(stderr);

      // Adicione mais detalhes sobre o cliente - use fmt::format
      // corretamente
      fmt::print(stderr, "Client pointer: {}\n", (void *)session.get());

      clients.push_back(std::make_unique<DebuggerClient>(session));
      clients.back()->AskFile();

      // Verifique se o cliente foi adicionado corretamente
      fmt::print(stderr, "Client added successfully. Total clients: {}\n",
                 clients.size());
      fflush(stderr);
    }
    else
    {
      fmt::print(stderr, "Client already exists in the list\n");
      fflush(stderr);
    }
  }
  catch (const std::exception &e)
  {
    fmt::print(stderr, "Exception during client addition: {}\n", e.what());
    fflush(stderr);
  }
}

void removeClientID(const TcpConnectionPtr &session)
{
  // Adicionar logging para debug
  fmt::print(stderr, "Attempting to remove client ID...\n");
  fflush(stderr);

  if (auto it = std::find_if(clients.begin(), clients.end(),
                             [&session](const auto &client)
                             {
                               return client->socket == session;
                             });
      it != clients.end())
  {
    fmt::print(stderr, "Client found, removing...\n");
    fflush(stderr);
    clients.erase(it);
    fmt::print(stderr, "Client removed successfully\n");
    fflush(stderr);
  }
  else
  {
    fmt::print(stderr, "Client not found in the list\n");
    fflush(stderr);
  }
}

// Manipulador de protocolo de debug para interpretar os comandos
// Essa implementação complementa o thread de debug

// Constantes para os comandos do protocolo
namespace DebugProtocol
{
  constexpr uint8_t CMD_CONNECT = 0xC5;    // 0xC5 observado nos logs (convertido de 0xC5 para decimal: 197)
  constexpr uint8_t CMD_DISCONNECT = 0xD1; // 0xD1 observado nos logs (convertido de 0xD1 para decimal: 209)

  // Outros possíveis comandos (a serem implementados conforme necessário)
  constexpr uint8_t CMD_PAUSE = 0x01;
  constexpr uint8_t CMD_RESUME = 0x02;
  constexpr uint8_t CMD_STEP = 0x03;
  constexpr uint8_t CMD_BREAKPOINT = 0x04;

  // Indica que qualquer pacote de tamanho 5 deve ser tratado como DISCONNECT
  constexpr size_t DISCONNECT_PACKET_SIZE = 5;
}

class DebugProtocolHandler
{
private:
  // Referência para o logger
  std::function<void(const std::string &)> logMessage;

public:
  DebugProtocolHandler(std::function<void(const std::string &)> logger)
      : logMessage(std::move(logger)) {}

  // Processa um comando e retorna o tipo de comando detectado
  uint8_t processCommand(const char *data, size_t length)
  {
    if (length == 0)
    {
      logMessage("Empty command received");
      return 0;
    }

    // Pega o primeiro byte como tipo de comando
    uint8_t commandType = static_cast<uint8_t>(data[0]);

    switch (commandType)
    {
    case DebugProtocol::CMD_CONNECT:
      logMessage("Tipo de comando detectado: CONNECT");
      break;

    case DebugProtocol::CMD_DISCONNECT:
      logMessage("Tipo de comando detectado: DISCONNECT");
      break;

      // Implemente outros casos conforme necessário

    default:
      logMessage(fmt::format("Tipo de comando desconhecido: {:#x}", commandType));
      break;
    }

    return commandType;
  }

  // Método específico para processar comando de conexão
  bool handleConnectCommand(const char *data, size_t length, const std::shared_ptr<TcpSession> &session)
  {
    if (length < 1 || static_cast<uint8_t>(data[0]) != DebugProtocol::CMD_CONNECT)
    {
      return false;
    }

    // Processar parâmetros do comando de conexão
    // Implementar conforme a estrutura do protocolo

    // Por exemplo, se o comando contiver um ID de cliente nos bytes 1-4:
    if (length >= 5)
    {
      uint32_t clientId = 0;
      std::memcpy(&clientId, data + 1, 4);
      logMessage(fmt::format("Iniciando conexão para cliente ID: {}", clientId));
    }

    // Responder ao cliente confirmando a conexão
    // Exemplo de resposta
    const char response[] = {static_cast<char>(DebugProtocol::CMD_CONNECT), 0x01}; // 0x01 indica sucesso
    session->send(response, sizeof(response));

    return true;
  }

  // Método específico para processar comando de desconexão
  bool handleDisconnectCommand(const char *data, size_t length, const std::shared_ptr<TcpSession> &session)
  {
    // Aceita tanto o comando específico quanto pacotes de tamanho 5
    if ((length >= 1 && static_cast<uint8_t>(data[0]) == DebugProtocol::CMD_DISCONNECT) ||
        (length == DebugProtocol::DISCONNECT_PACKET_SIZE))
    {

      logMessage("Processando comando de desconexão");

      // Log de todos os bytes do pacote para depuração
      std::string bytesLog = "Bytes do pacote de desconexão: ";
      for (size_t i = 0; i < length; ++i)
      {
        bytesLog += fmt::format("{:02x} ", static_cast<unsigned char>(data[i]));
      }
      logMessage(bytesLog);

      // Confirmar a desconexão ao cliente antes de finalizar
      const char response[] = {static_cast<char>(DebugProtocol::CMD_DISCONNECT), 0x01}; // 0x01 indica sucesso
      session->send(response, sizeof(response));

      // Retornar true para indicar que o cliente deve ser desconectado
      return true;
    }

    return false;
  }
};

// Exemplo de como integrar o manipulador de protocolo no thread de debug

void clientHandlerExample(
    std::shared_ptr<TcpSession> session,
    const std::function<void(const std::shared_ptr<TcpSession> &)> &markClientActive,
    const std::function<void(const std::shared_ptr<TcpSession> &)> &safeRemoveClient)
{

  auto logMessage = [](const std::string &message)
  {
    fmt::print(stderr, "[DEBUG] {}\n", message);
    fflush(stderr);
  };

  // Criar o manipulador de protocolo
  auto protocolHandler = std::make_shared<DebugProtocolHandler>(logMessage);

  // Modificar o callback de dados para usar o manipulador de protocolo
  // Isso seria parte do código em debugThread()
  session->set_data_callback(
      [session, markClientActive, safeRemoveClient, logMessage, protocolHandler](
          const char *data, size_t length)
      {
        markClientActive(session);

        // Processar o comando usando o manipulador de protocolo
        uint8_t commandType = protocolHandler->processCommand(data, length);

        // Tratar comandos específicos
        if (commandType == DebugProtocol::CMD_CONNECT)
        {
          protocolHandler->handleConnectCommand(data, length, session);
        }
        else if (commandType == DebugProtocol::CMD_DISCONNECT)
        {
          if (protocolHandler->handleDisconnectCommand(data, length, session))
          {
            // Se o comando de desconexão foi processado com sucesso, remover o cliente
            safeRemoveClient(session);
            return;
          }
        }

        // Continuar com o processamento normal de comandos para o cliente
        bool clientFound = false;
        for (const auto &client : clients)
        {
          if (client->socket == session)
          {
            clientFound = true;
            try
            {
              client->RecvCmd(data, length);
            }
            catch (const std::exception &e)
            {
              logMessage(fmt::format("Error processing client data: {}", e.what()));
              client->stopDebugging();
              safeRemoveClient(session);
            }
            break;
          }
        }

        if (!clientFound)
        {
          logMessage("Client not found for data callback");
          safeRemoveClient(session);
        }
      });
}

void debugThread()
{
  auto logMessage = [](const std::string &message)
  {
    fmt::print(stderr, "[DEBUG] {}\n", message);
    fflush(stderr);
  };

  try
  {
    asio::io_context io_context;

    // Estruturas de dados para gerenciar conexões ativas
    struct ClientInfo
    {
      std::shared_ptr<TcpSession> session;
      std::chrono::steady_clock::time_point lastActivity;
      bool isActive;
    };

    std::vector<ClientInfo> activeClients;
    std::mutex clientsMutex;

    // Lambda para remover cliente de forma segura
    auto safeRemoveClient = [&activeClients, &clientsMutex, &logMessage](
                                const std::shared_ptr<TcpSession> &session)
    {
      std::unique_lock<std::mutex> lock(clientsMutex);

      logMessage(fmt::format("Removing client. Current clients count: {}", activeClients.size()));

      auto it = std::find_if(
          activeClients.begin(),
          activeClients.end(),
          [&session](const auto &client)
          { return client.session == session; });

      if (it != activeClients.end())
      {
        // Primeira para o debugger do cliente - CORRIGIDO: verificar se clients está definido
        if (!clients.empty()) // Adicionar esta verificação
        {
          auto clientIt = std::find_if(
              clients.begin(), clients.end(), [&session](const auto &client)
              { return client->socket == session; });

          if (clientIt != clients.end())
          {
            logMessage("Found client in global list, stopping debugging");
            (*clientIt)->stopDebugging();
          }
          else
          {
            logMessage("Client not found in global list");
          }

          // Remove o cliente da lista global - CORRIGIDO: somente se estiver presente
          if (clientIt != clients.end())
          {
            removeClientID(session);
          }
        }

        // Remove o cliente da lista local
        logMessage(fmt::format("Removing client from position {}", std::distance(activeClients.begin(), it)));
        activeClients.erase(it);

        logMessage(fmt::format("Client removed. Current clients count: {}", activeClients.size()));
      }
      else
      {
        logMessage("Client not found in active clients list");
      }
    };

    // Lambda para marcar cliente como ativo
    auto markClientActive = [&activeClients, &clientsMutex, &logMessage](
                                const std::shared_ptr<TcpSession> &session)
    {
      std::unique_lock<std::mutex> lock(clientsMutex);
      auto it = std::find_if(
          activeClients.begin(),
          activeClients.end(),
          [&session](auto &client)
          { return client.session == session; });

      if (it != activeClients.end())
      {
        it->lastActivity = std::chrono::steady_clock::now();
        it->isActive = true;
        logMessage(fmt::format("Client marked as active"));
      }
      else
      {
        logMessage(fmt::format("Client not found when marking as active"));
      }
    };

    // Timer para verificar clientes inativos
    auto checkTimer = std::make_shared<asio::steady_timer>(io_context);

    std::function<void()> checkInactiveClients;
    checkInactiveClients = [&activeClients,
                            &clientsMutex,
                            &checkTimer,
                            &safeRemoveClient,
                            &checkInactiveClients,
                            &logMessage]()
    {
      auto now = std::chrono::steady_clock::now();
      std::vector<std::shared_ptr<TcpSession>> clientsToRemove;

      {
        std::unique_lock<std::mutex> lock(clientsMutex);
        logMessage(fmt::format("Checking inactive clients. Total clients: {}", activeClients.size()));

        for (const auto &client : activeClients)
        {
          auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                             now - client.lastActivity)
                             .count();
          if (elapsed > 10)
          {
            logMessage(fmt::format(
                "Client inactive for {} seconds, marking for removal", elapsed));
            clientsToRemove.push_back(client.session);
          }
        }
      }

      for (const auto &client : clientsToRemove)
      {
        safeRemoveClient(client);
      }

      checkTimer->expires_after(std::chrono::seconds(5));
      checkTimer->async_wait(
          [&checkInactiveClients, &logMessage](const std::error_code &ec)
          {
            if (!ec)
            {
              checkInactiveClients();
            }
            else
            {
              logMessage(fmt::format("Timer error: {}", ec.message()));
            }
          });
    };

    // Inicia o timer de verificação
    checkTimer->expires_after(std::chrono::seconds(5));
    checkTimer->async_wait([&checkInactiveClients, &logMessage](const std::error_code &ec)
                           {
      if (!ec)
      {
        checkInactiveClients();
      }
      else
      {
        logMessage(fmt::format("Initial timer error: {}", ec.message()));
      } });

    // Configura e inicia o acceptor
    tcp::acceptor acceptor(io_context,
                           tcp::endpoint(tcp::v4(), SM_Debugger_port()));
    acceptor.set_option(tcp::acceptor::reuse_address(true));

    logMessage(
        fmt::format("Debugger listening on port {}", SM_Debugger_port()));

    // Loop principal de aceitação de conexões
    while (true)
    {
      try
      {
        tcp::socket socket(io_context);
        acceptor.accept(socket);

        socket.set_option(tcp::no_delay(true));

        auto session = TcpSession::create(std::move(socket));

        {
          std::unique_lock<std::mutex> lock(clientsMutex);
          activeClients.push_back(
              {session, std::chrono::steady_clock::now(), true});
        }

        logMessage(fmt::format("New client accepted. Total active clients: {}", activeClients.size()));

        // Configura o callback de desconexão
        session->set_disconnect_callback(
            [session, safeRemoveClient, logMessage]()
            {
              logMessage("Client disconnect callback triggered");
              safeRemoveClient(session);
            });

        // Configura o callback de dados
        session->set_data_callback(
            [session, markClientActive, safeRemoveClient, logMessage](
                const char *data, size_t length)
            {
              markClientActive(session);

              // Log detalhado do pacote recebido
              std::string hexDump = fmt::format("[PACKET] Tamanho: {} bytes | HexDump: ", length);
              if (length > 0)
              {
                for (size_t i = 0; i < length; ++i)
                {
                  hexDump += fmt::format("{:02x} ", static_cast<unsigned char>(data[i]));
                }
              }
              else
              {
                hexDump += "<pacote vazio>";
              }
              logMessage(hexDump);

              // Se for pacote de desconexão (5 bytes), mostrar aviso específico
              if (length == 5)
              {
                logMessage("Detectado pacote de DISCONNECT (5 bytes)");
                safeRemoveClient(session);
                return;
              }

              // Processar dados do cliente
              bool clientFound = false;
              if (!clients.empty())
              {
                for (const auto &client : clients)
                {
                  if (client && client->socket == session)
                  {
                    clientFound = true;
                    try
                    {
                      logMessage("Processando comando para cliente");
                      client->RecvCmd(data, length);
                      logMessage("Comando processado com sucesso");
                    }
                    catch (const std::exception &e)
                    {
                      logMessage(fmt::format("Erro processando dados do cliente: {}", e.what()));
                      client->stopDebugging();
                      safeRemoveClient(session);
                    }
                    break;
                  }
                }
              }

              if (!clientFound)
              {
                logMessage("Cliente não encontrado para callback de dados");
                safeRemoveClient(session);
              }
            });

        try
        {
          addClientID(session);
          logMessage("Client added to global list successfully");
          session->start();
          logMessage("Session started successfully");
        }
        catch (const std::exception &e)
        {
          logMessage(fmt::format("Error initializing client: {}", e.what()));
          safeRemoveClient(session);
        }
      }
      catch (const std::exception &e)
      {
        logMessage(fmt::format("Error accepting connection: {}", e.what()));
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
    }

    io_context.run();
  }
  catch (const std::exception &e)
  {
    logMessage(fmt::format("Fatal error in debug thread: {}", e.what()));
  }
}

/**
 * @brief Called on debug spew.
 *
 * @param msg    Message text.
 * @param fmt    Message formatting arguments (printf-style).
 */
void DebugReport::OnDebugSpew(const char *msg, ...)
{
  va_list ap;
  char buffer[512];

  va_start(ap, msg);
  ke::SafeVsprintf(buffer, sizeof(buffer), msg, ap);
  va_end(ap);
  original->OnDebugSpew(buffer);
}

/**
 * @brief Called when an error is reported and no exception
 * handler was available.
 *
 * @param report  Error report object.
 * @param iter      Stack frame iterator.
 */
void DebugReport::ReportError(const IErrorReport &report, IFrameIterator &iter)
{
  if (!clients.empty())
  {
    auto plugin = report.Context();
    if (plugin)
    {
      auto found = false;
      /* first search already found attached hook */
      for (auto &client : clients)
      {
        if (client && client->context_ == iter.Context())
        {
          found = true;
          client->ReportError(report, iter);
          break;
        }
      }

      /* if not found, search for new client who wants to attach to
       * current file */
      if (!found)
      {
        for (int i = 0; i < report.Context()
                                ->GetRuntime()
                                ->GetDebugInfo()
                                ->NumFiles();
             i++)
        {
          auto filename = std::string(report.Context()
                                          ->GetRuntime()
                                          ->GetDebugInfo()
                                          ->GetFileName(i));

          auto current_file = std::filesystem::path(filename).filename().string();
          lowercase(current_file);

          for (auto &client : clients)
          {
            if (client->files.find(current_file) != client->files.end())
            {
              client->ReportError(report, iter);
            }
          }
        }
      }
    }
  }

  // Add debug logging for VSCode extension requests on error
  if (DEBUG == 1)
  {
    fmt::print("VSCode extension request: {}\n", report.Message());
  }

  original->ReportError(report, iter);
}

void(DebugHandler)(SourcePawn::IPluginContext *IPlugin,
                   sp_debug_break_info_t &BreakInfo,
                   const SourcePawn::IErrorReport *IErrorReport)
{
  if (!IPlugin->IsDebugging())
    return;

  if (!clients.empty())
  {
    /* first search already found attached hook */
    for (auto it = clients.begin(); it != clients.end(); ++it)
    {
      const auto &client = *it;
      if (client && client->context_ == IPlugin)
      {
        try
        {
          client->DebugHook(IPlugin, BreakInfo);
        }
        catch (DebuggerClient::debugger_stopped &ex)
        {
          // it = clients.begin();
          // continue;
          return;
        }
      }
    }

    for (int i = 0; i < IPlugin->GetRuntime()->GetDebugInfo()->NumFiles();
         i++)
    {
      auto filename = IPlugin->GetRuntime()->GetDebugInfo()->GetFileName(i);
      auto current_file = std::filesystem::path(filename).filename().string();
      lowercase(current_file);

      for (auto it = clients.begin(); it != clients.end(); ++it)
      {
        const auto &client = *it;
        if (client->files.find(current_file) != client->files.end())
        {
          try
          {
            client->DebugHook(IPlugin, BreakInfo);
          }
          catch (DebuggerClient::debugger_stopped &ex)
          {
            return;
          }
        }
      }
    }
  }
}