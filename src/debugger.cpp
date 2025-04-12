#include "debugger.h"
#include "utlbuffer.h"
#include <assert.h>
#include <ctype.h>
#include <deque>
#include <filesystem>
#include <fmt/printf.h>
#include <fstream>
#include <mutex>
#include <thread>
#include <setjmp.h>
#include <signal.h>
#include <sstream>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifndef DEBUG
#define DEBUG 1
#endif

#include <asio.hpp>
#include <sourcepawn/include/sp_vm_types.h>
#include <nlohmann/json.hpp>

using namespace sp;
using namespace asio;
using namespace asio::ip;
using json = nlohmann::json;

//
//  Lowercases string
//
template <typename T>
std::basic_string<T> lowercase(const std::basic_string<T>& s)
{
    std::basic_string<T> s2 = s;
    std::transform(s2.begin(), s2.end(), s2.begin(), tolower);
    return std::move(s2);
}

enum DebugState {
    DebugDead = -1,
    DebugRun = 0,
    DebugBreakpoint,
    DebugPause,
    DebugStepIn,
    DebugStepOver,
    DebugStepOut,
    DebugException
};

enum MessageType {
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

std::vector<std::string> split_string(const std::string& str, const std::string& delimiter)
{
    std::vector<std::string> strings;

    std::string::size_type pos = 0;
    std::string::size_type prev = 0;
    while ((pos = str.find(delimiter, prev)) != std::string::npos) {
        strings.push_back(str.substr(prev, pos - prev));
        prev = pos + 1;
    }

    strings.push_back(str.substr(prev));
    return strings;
}

DebugReport DebugListener;

class DebuggerClient {
public:
    std::shared_ptr<tcp::socket> socket;
    std::unordered_set<std::string> files;
    int DebugState = 0;

    struct variable_s {
        std::string name;
        std::string value;
        std::string type;
    };

    struct call_stack_s {
        uint32_t line;
        std::string name;
        std::string filename;
    };

    struct breakpoint_s {
        long line;
        std::string filename;
    };

public:
    bool unload = false;
    bool receive_walk_cmd = false;
    std::mutex mtx;
    std::condition_variable cv;
    SourcePawn::IPluginContext* context_;
    uint32_t current_line;
    std::unordered_map<std::string, std::unordered_set<long>> break_list;
    int current_state = 0;
    cell_t lastfrm_ = 0;
    cell_t cip_;
    cell_t frm_;
    std::map<std::string, std::shared_ptr<SmxV1Image>> images;
    std::shared_ptr<SmxV1Image> current_image = nullptr;
    SourcePawn::IFrameIterator* debug_iter;
    std::vector<std::unique_ptr<DebuggerClient>> clients;
    std::mutex clientsMutex;

    DebuggerClient(std::shared_ptr<tcp::socket> socket) : socket(socket) {}

    ~DebuggerClient()
    {
        stopDebugging();
        fmt::print("Debugger disabled.\n");
    }

    class debugger_stopped : public std::exception {
    public:
        const char* what() const throw()
        {
            return "Debugger exited!";
        }
    };

    void setBreakpoint(std::string path, int line, int id)
    {
        break_list[path].insert(line);
    }

    void clearBreakpoints(std::string fileName)
    {
        auto found = break_list.find(fileName);
        if (found != break_list.end()) {
            found->second.clear();
        }
    }

    enum {
        DISP_DEFAULT = 0x10,
        DISP_STRING = 0x20,
        DISP_BIN = 0x30,
        DISP_HEX = 0x40,
        DISP_BOOL = 0x50,
        DISP_FIXED = 0x60,
        DISP_FLOAT = 0x70
    };
#define MAX_DIMS 3
#define DISP_MASK 0x0f

    char* get_string(SmxV1Image::Symbol* sym)
    {
        assert(sym->ident() == sp::IDENT_ARRAY || sym->ident() == sp::IDENT_REFARRAY);
        assert(sym->dimcount() == 1);

        cell_t* addr;
        cell_t base = sym->addr();
        if (sym->vclass() == 1 || sym->vclass() == 3)
            base += frm_;
        if (sym->ident() == sp::IDENT_REFARRAY) {
            context_->LocalToPhysAddr(base, &addr);
            assert(addr != nullptr);
            base = *addr;
        }

        char* str;
        if (context_->LocalToStringNULL(base, &str) != SP_ERROR_NONE)
            return nullptr;
        return str;
    }

    int get_symbolvalue(const SmxV1Image::Symbol* sym, int index, cell_t* value)
    {
        cell_t* vptr;
        cell_t base = sym->addr();
        if (sym->vclass() & DISP_MASK)
            base += frm_;

        if (sym->ident() == sp::IDENT_REFERENCE || sym->ident() == sp::IDENT_REFARRAY) {
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

    void printvalue(long value, int disptype, std::string& out_value, std::string& out_type)
    {
        char out[64];
        if (disptype == DISP_FLOAT) {
            out_type = "float";
            sprintf(out, "%f", sp_ctof(value));
        }
        else if (disptype == DISP_FIXED) {
            out_type = "fixed";
#define MULTIPLIER 1000
            long ipart = value / MULTIPLIER;
            value -= MULTIPLIER * ipart;
            if (value < 0)
                value = -value;
            sprintf(out, "%ld.%03ld", ipart, value);
        }
        else if (disptype == DISP_HEX) {
            out_type = "hex";
            sprintf(out, "%lx", value);
        }
        else if (disptype == DISP_BOOL) {
            out_type = "bool";
            switch (value) {
            case 0:
                sprintf(out, "false");
                break;
            case 1:
                sprintf(out, "true");
                break;
            default:
                sprintf(out, "%ld (true)", value);
                break;
            }
        }
        else {
            out_type = "cell";
            sprintf(out, "%ld", value);
        }
        out_value += out;
    }

    nlohmann::json read_variable(uint32_t& addr, uint32_t type_id, debug::Rtti* rtti, bool is_ref = false)
    {
        nlohmann::json json;
        if (!rtti) {
            rtti = const_cast<debug::Rtti*>(current_image->rtti_data()->typeFromTypeId(type_id));
        }
        cell_t* ptr;
        switch (rtti->type()) {
        case cb::kAny: {
            context_->LocalToPhysAddr(addr, &ptr);
            json = (int32_t)*ptr;
        }
        case cb::kBool: {
            context_->LocalToPhysAddr(addr, &ptr);
            json = (bool)*ptr;
            break;
        }
        case cb::kInt32: {
            context_->LocalToPhysAddr(addr, &ptr);
            json = (int32_t)*ptr;
            break;
        }
        case cb::kFloat32: {
            context_->LocalToPhysAddr(addr, &ptr);
            json = sp_ctof(*ptr);
            break;
        }
        case cb::kFixedArray: {
            if (rtti->inner()) {
                if (rtti->inner()->type() == cb::kChar8) {
                    json = read_variable(addr, rtti->inner()->type(), const_cast<debug::Rtti*>(rtti->inner()), false);
                }
                else {
                    for (int i = 0; i < rtti->index(); i++) {
                        uint32_t start = addr;
                        json[i] = read_variable(start, rtti->inner()->type(), const_cast<debug::Rtti*>(rtti->inner()), false);
                        addr += 4;
                    }
                }
            }
            break;
        }
        case cb::kChar8: {
            char* str = nullptr;
            if (context_->LocalToStringNULL(addr, &str) != SP_ERROR_NONE) {
                break;
            }
            if (str) {
                addr += strlen(str) + 1;
            }
            if (addr % sizeof(cell_t) != 0) {
                addr += sizeof(cell_t) - (addr % sizeof(cell_t));
            }
            json = str ? str : "";
            break;
        }
        case cb::kArray: {
            if (is_ref) {
                cell_t* a;
                context_->LocalToPhysAddr(addr, &a);
                addr = *a;
            }
            if (rtti->inner()) {
                json = read_variable(addr, rtti->inner()->type(), const_cast<debug::Rtti*>(rtti->inner()));
            }
            break;
        }
        case cb::kEnumStruct: {
            auto fields = current_image->getEnumFields(rtti->index());
            uint32_t start = addr;
            for (auto& field : fields) {
                auto name = current_image->GetDebugName(field->name);
                auto rtti_field = current_image->rtti_data()->typeFromTypeId(field->type_id);
                if (!rtti_field) {
                    break;
                }
                json[name] = read_variable(start, rtti_field->type(), (sp::debug::Rtti*)rtti_field);
            }
            break;
        }
        case cb::kClassdef: {
            auto fields = current_image->getTypeFields(rtti->index());
            cell_t* ptr;
            uint32_t field_offset = addr;
            for (auto& field : fields) {
                uint32_t start = field_offset;
                auto name = current_image->GetDebugName(field->name);
                auto rtti_field = current_image->rtti_data()->typeFromTypeId(field->type_id);
                json[name] = read_variable(start, rtti_field->type(), (sp::debug::Rtti*)rtti_field, true);
                field_offset += sizeof(cell_t);
            }
            break;
        }
        }
        return json;
    }

    variable_s display_variable(SmxV1Image::Symbol* sym, uint32_t index[], int idxlevel, bool noarray = false)
    {
        nlohmann::json json;
        variable_s var;
        var.name = "N/A";
        if (current_image->GetDebugName(sym->name()) != nullptr) {
            var.name = current_image->GetDebugName(sym->name());
        };
        var.type = "N/A";
        var.value = "";
        cell_t value;
        std::unique_ptr<std::vector<SmxV1Image::ArrayDim*>> symdims;
        assert(index != NULL);
        auto rtti = sym->rtti();
        if (rtti && rtti->type_id) {
            uint32_t base = static_cast<uint32_t>(rtti->address);
            if (sym->vclass() == 1 || sym->vclass() == 3)
                base += frm_;

            try {
                auto json = read_variable(base, rtti->type_id, nullptr, sym->vclass() == 0x3);
                if (!json.empty()) {
                    var.value = json.dump();
                    return var;
                }
            }
            catch (...) {
                // skip rtti parse
            }
        }

        if ((uint32_t)cip_ < sym->codestart() || (uint32_t)cip_ > sym->codeend()) {
            var.value = "Not in scope.";
            return var;
        }

        if ((sym->vclass() & ~DISP_MASK) == 0) {
            const char* tagname = current_image->GetTagName(sym->tagid());
            if (tagname != nullptr) {
                if (!stricmp(tagname, "bool")) {
                    sym->setVClass(sym->vclass() | DISP_BOOL);
                }
                else if (!stricmp(tagname, "float")) {
                    sym->setVClass(sym->vclass() | DISP_FLOAT);
                }
            }
            if ((sym->vclass() & ~DISP_MASK) == 0 && (sym->ident() == sp::IDENT_ARRAY || sym->ident() == sp::IDENT_REFARRAY) && sym->dimcount() == 1) {
                char* ptr = get_string(sym);
                if (ptr != nullptr) {
                    uint32_t i;
                    for (i = 0; ptr[i] != '\0'; i++) {
                        if ((ptr[i] < ' ' && ptr[i] != '\n' && ptr[i] != '\r' && ptr[i] != '\t'))
                            break;
                        if (i == 0 && !isalpha(ptr[i]))
                            break;
                    }
                    if (i > 0 && ptr[i] == '\0')
                        sym->setVClass(sym->vclass() | DISP_STRING);
                }
            }
        }

        if (sym->ident() == sp::IDENT_ARRAY || sym->ident() == sp::IDENT_REFARRAY) {
            int dim;
            symdims = std::make_unique<std::vector<SmxV1Image::ArrayDim*>>(*current_image->GetArrayDimensions(sym));
            for (dim = 0; dim < idxlevel; dim++) {
                if (symdims->at(dim)->size() > 0 && index[dim] >= symdims->at(dim)->size())
                    break;
            }
            if (dim < idxlevel) {
                var.value = "(index out of range)";
                return var;
            }
        }

        if ((sym->ident() == sp::IDENT_ARRAY || sym->ident() == sp::IDENT_REFARRAY) && idxlevel == 0) {
            if ((sym->vclass() & ~DISP_MASK) == DISP_STRING) {
                var.type = "String";
                char* str = get_string(sym);
                if (str != nullptr) {
                    var.value = str;
                }
                else
                    var.value = "NULL_STRING";
            }
            else if (sym->dimcount() == 1) {
                if (!noarray)
                    var.type = "Array";
                assert(symdims != nullptr);
                uint32_t len = symdims->at(0)->size();
                uint32_t i;
                auto type = (sym->vclass() & ~DISP_MASK);
                if (type == DISP_FLOAT) {
                    json = std::vector<float>();
                }
                else if (type == DISP_BOOL) {
                    json = std::vector<bool>();
                }
                else {
                    json = std::vector<cell_t>();
                }
                for (i = 0; i < len; i++) {
                    if (get_symbolvalue(sym, i, &value)) {
                        if (type == DISP_FLOAT) {
                            json.push_back(sp_ctof(value));
                        }
                        else if (type == DISP_BOOL) {
                            json.push_back(value);
                        }
                        else {
                            json.push_back(value);
                        }
                    }
                }
                var.value = json.dump(4).c_str();
            }
            else {
                var.value = "(multi-dimensional array)";
            }
        }
        else if (sym->ident() != sp::IDENT_ARRAY && sym->ident() != sp::IDENT_REFARRAY && idxlevel > 0) {
            var.value = "(invalid index, not an array)";
        }
        else {
            assert(idxlevel > 0 || index[0] == 0);
            int dim;
            int base = 0;
            for (dim = 0; dim < idxlevel - 1; dim++) {
                if (!noarray)
                    var.type = "Array";
                base += index[dim];
                if (!get_symbolvalue(sym, base, &value))
                    break;
                base += value / sizeof(cell_t);
            }

            if (get_symbolvalue(sym, base + index[dim], &value) && sym->dimcount() == idxlevel)
                printvalue(value, (sym->vclass() & ~DISP_MASK), var.value, var.type);
            else if (sym->dimcount() != idxlevel)
                var.value = "(invalid number of dimensions)";
            else
                var.value = "(?)";
        }
        return var;
    }

    void evaluateVar(int frame_id, char* variable)
    {
        if (current_state != DebugRun) {
            auto imagev1 = current_image.get();

            std::unique_ptr<SmxV1Image::Symbol> sym;
            if (imagev1->GetVariable(variable, cip_, sym)) {
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
                    buffer.PutInt(var.type.size() + 1);
                    buffer.PutString(var.type.c_str());
                    buffer.PutInt(0);
                }
                *(uint32_t*)buffer.Base() = buffer.TellPut() - 5;
                sendData(static_cast<const char*>(buffer.Base()), static_cast<size_t>(buffer.TellPut()));
            }
        }
    }

    int set_symbolvalue(const SmxV1Image::Symbol* sym, int index, cell_t value)
    {
        cell_t* vptr;
        cell_t base = sym->addr();
        if (sym->vclass() & DISP_MASK)
            base += frm_;

        if (sym->ident() == sp::IDENT_REFERENCE || sym->ident() == sp::IDENT_REFARRAY) {
            context_->LocalToPhysAddr(base, &vptr);
            assert(vptr != nullptr);
            base = *vptr;
        }

        context_->LocalToPhysAddr(base + index * sizeof(cell_t), &vptr);
        assert(vptr != nullptr);
        *vptr = value;
        return true;
    }

    bool SetSymbolString(const SmxV1Image::Symbol* sym, char* str)
    {
        assert(sym->ident() == sp::IDENT_ARRAY || sym->ident() == sp::IDENT_REFARRAY);
        assert(sym->dimcount() == 1);

        cell_t* vptr;
        cell_t base = sym->addr();
        if (sym->vclass() & DISP_MASK)
            base += frm_;

        if (sym->ident() == sp::IDENT_REFERENCE || sym->ident() == sp::IDENT_REFARRAY) {
            context_->LocalToPhysAddr(base, &vptr);
            assert(vptr != nullptr);
            base = *vptr;
        }

        std::unique_ptr<std::vector<SmxV1Image::ArrayDim*>> dims;
        dims = std::make_unique<std::vector<SmxV1Image::ArrayDim*>>(*current_image->GetArrayDimensions(sym));
        return context_->StringToLocalUTF8(base, dims->at(0)->size(), str, NULL) == SP_ERROR_NONE;
    }

    void setVariable(std::string var, std::string value, int index)
    {
        bool success = false;
        bool valid_value = true;
        if (current_state != DebugRun) {
            auto imagev1 = current_image.get();
            std::unique_ptr<SmxV1Image::Symbol> sym;
            cell_t result = 0;
            value.erase(remove(value.begin(), value.end(), '\"'), value.end());
            if (imagev1->GetVariable(var.c_str(), cip_, sym)) {
                if ((sym->ident() == IDENT_ARRAY || sym->ident() == IDENT_REFARRAY)) {
                    if ((sym->vclass() & ~DISP_MASK) == DISP_STRING) {
                        SetSymbolString(sym.get(), const_cast<char*>(value.c_str()));
                    }
                    valid_value = false;
                }
                else {
                    size_t lastChar;
                    try {
                        int intvalue = std::stoi(value, &lastChar);
                        if (lastChar == value.size()) {
                            result = intvalue;
                        }
                        else {
                            auto val = std::stof(value, &lastChar);
                            result = sp_ftoc(val);
                        }
                    }
                    catch (...) {
                        if (value == "true") {
                            result = 1;
                        }
                        else if (value == "false") {
                            result = 0;
                        }
                        else {
                            valid_value = false;
                        }
                    }
                }

                if (valid_value && (imagev1->GetVariable(var.c_str(), cip_, sym))) {
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
        *(uint32_t*)buffer.Base() = buffer.TellPut() - 5;
        sendData(static_cast<const char*>(buffer.Base()), static_cast<size_t>(buffer.TellPut()));
    }

    void sendVariables(char* scope)
    {
        bool local_scope = strstr(scope, ":%local%");
        bool global_scope = strstr(scope, ":%global%");
        if (current_state != DebugRun) {
            auto imagev1 = current_image.get();

            std::unique_ptr<SmxV1Image::Symbol> sym;
            if (current_image && imagev1) {
#define sDIMEN_MAX 4
                uint32_t idx[sDIMEN_MAX], dim;
                dim = 0;
                memset(idx, 0, sizeof idx);
                std::vector<variable_s> vars;
                if (local_scope || global_scope) {
                    SmxV1Image::SymbolIterator iter = imagev1->symboliterator(global_scope);
                    while (!iter.Done()) {
                        const auto sym = iter.Next();

                        if (sym->ident() != sp::IDENT_FUNCTION && (sym->codestart() <= (uint32_t)cip_ && sym->codeend() >= (uint32_t)cip_) || global_scope) {
                            auto var = display_variable(sym, idx, dim);
                            if (local_scope) {
                                if ((sym->vclass() & DISP_MASK) > 0) {
                                    vars.push_back(var);
                                }
                            }
                            else {
                                if (!((sym->vclass() & DISP_MASK) > 0)) {
                                    vars.push_back(var);
                                }
                            }
                        }
                    }
                }
                else {
                    if (imagev1->GetVariable(scope, cip_, sym)) {
                        auto var = display_variable(sym.get(), idx, dim, true);
                        std::string var_name = scope;
                        auto values = split_string(var.value, ",");
                        int i = 0;
                        for (auto val : values) {
                            vars.push_back({ std::to_string(i), val, var.type });
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
                for (auto var : vars) {
                    buffer.PutInt(var.name.size() + 1);
                    buffer.PutString(var.name.c_str());
                    buffer.PutInt(var.value.size() + 1);
                    buffer.PutString(var.value.c_str());
                    buffer.PutInt(var.type.size() + 1);
                    buffer.PutString(var.type.c_str());
                    buffer.PutInt(0);
                }
                *(uint32_t*)buffer.Base() = buffer.TellPut() - 5;
                sendData(static_cast<const char*>(buffer.Base()), static_cast<size_t>(buffer.TellPut()));
            }
        }
    }

    void CallStack()
    {
        std::vector<call_stack_s> callStack;
        if (current_state == DebugException) {
            if (debug_iter) {
                uint32_t index = 0;
                for (; !debug_iter->Done(); debug_iter->Next(), index++) {
                    if (debug_iter->IsNativeFrame()) {
                        callStack.push_back({ 0, debug_iter->FunctionName(), "native" });
                    }
                    else if (debug_iter->IsScriptedFrame()) {
                        auto current_file = std::filesystem::path(debug_iter->FilePath()).filename().string();
                        lowercase(current_file);
                        callStack.push_back({ debug_iter->LineNumber() - 1, debug_iter->FunctionName(), current_file });
                    }
                }
            }
            current_state = DebugBreakpoint;
        }
        else if (current_state != DebugRun) {
            IFrameIterator* iter = context_->CreateFrameIterator();

            uint32_t index = 0;
            for (; !iter->Done(); iter->Next(), index++) {
                if (iter->IsNativeFrame()) {
                    callStack.push_back({ 0, iter->FunctionName(), "" });
                }
                else if (iter->IsScriptedFrame()) {
                    std::string current_file = iter->FilePath();
                    for (auto file : files) {
                        if (file.find(current_file) != std::string::npos) {
                            current_file = file;
                            break;
                        }
                    }
                    callStack.push_back({ iter->LineNumber() - 1, iter->FunctionName(), current_file });
                }
            }
            context_->DestroyFrameIterator(iter);
        }

        CUtlBuffer buffer;
        buffer.PutUnsignedInt(0);
        {
            buffer.PutChar(MessageType::CallStack);
            buffer.PutInt(callStack.size());
            for (auto stack : callStack) {
                buffer.PutInt(stack.name.size() + 1);
                buffer.PutString(stack.name.c_str());
                buffer.PutInt(stack.filename.size() + 1);
                buffer.PutString(stack.filename.c_str());
                buffer.PutInt(stack.line + 1);
            }
        }
        *(uint32_t*)buffer.Base() = buffer.TellPut() - 5;
        sendData(static_cast<const char*>(buffer.Base()), static_cast<size_t>(buffer.TellPut()));
    }

    void WaitWalkCmd(std::string reason = "Breakpoint", std::string text = "N/A")
    {
        if (!receive_walk_cmd) {
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
                *(uint32_t*)buffer.Base() = buffer.TellPut() - 5;
            }
            sendData(static_cast<const char*>(buffer.Base()), static_cast<size_t>(buffer.TellPut()));
            std::unique_lock<std::mutex> lck(mtx);
            cv.wait(lck, [this] { return receive_walk_cmd; });
        }
        if (current_state == DebugDead) {
            unload = true;
            throw debugger_stopped();
        }
    }

    void ReportError(const IErrorReport& report, IFrameIterator& iter)
    {
        receive_walk_cmd = false;
        current_state = DebugException;
        context_ = iter.Context();
        debug_iter = &iter;
        WaitWalkCmd("exception", report.Message());
    }

    int DebugHook(SourcePawn::IPluginContext* ctx, sp_debug_break_info_t& BreakInfo)
    {
        std::string filename = ctx->GetRuntime()->GetFilename();
        auto image = images.find(filename);
        if (image == images.end()) {
            FILE* fp = fopen(filename.c_str(), "rb");
            current_image = std::make_shared<SmxV1Image>(fp);
            current_image->validate();
            images.insert({ filename, current_image });
            fclose(fp);
        }
        else {
            current_image = image->second;
        }
        context_ = ctx;
        if (current_state == DebugDead)
            return current_state;

        context_ = ctx;
        cip_ = BreakInfo.cip;
        frm_ = BreakInfo.frm;
        receive_walk_cmd = false;

        IFrameIterator* iter = context_->CreateFrameIterator();
        std::string current_file = "N/A";
        uint32_t index = 0;
        for (; !iter->Done(); iter->Next(), index++) {
            if (iter->IsNativeFrame()) {
                continue;
            }

            if (iter->IsScriptedFrame()) {
                current_file = std::filesystem::path(iter->FilePath()).filename().string();
                lowercase(current_file);

                for (auto file : files) {
                    if (file.find(current_file) != std::string::npos) {
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

        if (current_line == lastline)
            return current_state;

        lastline = current_line;
        if (current_state == DebugStepOut && frm_ > lastfrm_)
            current_state = DebugStepIn;

        if (current_state == DebugPause || current_state == DebugStepIn) {
            WaitWalkCmd();
        }
        else {
            auto found = break_list.find(current_file);
            if (found != break_list.end()) {
                if (found->second.find(current_line) != found->second.end()) {
                    current_state = DebugBreakpoint;
                    WaitWalkCmd();
                }
            }
        }

        if (current_state == DebugStepOver) {
            if (frm_ < lastfrm_) {
                return current_state;
            }

            WaitWalkCmd();

            if (current_state == DebugDead)
                return current_state;
        }

        lastfrm_ = frm_;

        return current_state;
    }

    void SwitchState(unsigned char state)
    {
        current_state = state;
        receive_walk_cmd = true;
        cv.notify_one();
    }

    void AskFile()
    {
    }

    void RecvDebugFile(CUtlBuffer* buf)
    {
        char file[260];
        int strlen = buf->GetInt();
        buf->GetString(file, strlen);
        auto filename = std::filesystem::path(file).filename().string();
        lowercase(filename);
        files.insert(filename);
    }

    void RecvStateSwitch(CUtlBuffer* buf)
    {
        auto CurrentState = buf->GetUnsignedChar();
        SwitchState(CurrentState);
    }

    void RecvCallStack(CUtlBuffer* buf)
    {
        CallStack();
    }

    void recvRequestVariables(CUtlBuffer* buf)
    {
        char scope[256];
        int strlen = buf->GetInt();
        buf->GetString(scope, strlen);
        sendVariables(scope);
    }

    void recvRequestEvaluate(CUtlBuffer* buf)
    {
        int frameId;
        char variable[256];
        int strlen = buf->GetInt();
        buf->GetString(variable, strlen);
        frameId = buf->GetInt();
        evaluateVar(frameId, variable);
    }

    void recvDisconnect(CUtlBuffer* buf)
    {

        std::lock_guard<std::mutex> lock(clientsMutex);
        if (auto it = std::find_if(clients.begin(), clients.end(),
            [this](const auto& client) {
                return client->socket == this->socket;
            });
            it != clients.end()) {
            clients.erase(it);
        }
        stopDebugging();
    }

    void recvBreakpoint(CUtlBuffer* buf)
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

    void recvClearBreakpoints(CUtlBuffer* buf)
    {
        char path[256];
        int strlen = buf->GetInt();
        buf->GetString(path, strlen);

        std::string filename(std::filesystem::path(path).filename().string());
        lowercase(filename);
        clearBreakpoints(filename);
    }

    void stopDebugging()
    {
        current_state = DebugDead;
        receive_walk_cmd = true;
        cv.notify_one();
        std::unique_lock<std::mutex> lck(mtx);
        cv.wait(lck, [this] { return unload; });
    }

    void recvStopDebugging(CUtlBuffer* buf)
    {
        stopDebugging();
        std::lock_guard<std::mutex> lock(clientsMutex);
        if (auto it = std::find_if(clients.begin(), clients.end(),
            [this](const auto& client) {
                return client->socket == this->socket;
            });
            it != clients.end()) {
            clients.erase(it);
        }
    }
    void recvRequestSetVariable(CUtlBuffer* buf)
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

    void RecvCmd(const char* buffer, size_t len)
    {
        CUtlBuffer buf((void*)buffer, len);
        while (buf.TellGet() < len) {
            int msg_len = buf.GetUnsignedInt();
            int type = buf.GetUnsignedChar();
            switch (type) {
            case RequestFile:
                RecvDebugFile(&buf);
                break;
            case Pause:
                RecvStateSwitch(&buf);
                break;
            case Continue:
                RecvStateSwitch(&buf);
                break;
            case StepIn:
                RecvStateSwitch(&buf);
                break;
            case StepOver:
                RecvStateSwitch(&buf);
                break;
            case StepOut:
                RecvStateSwitch(&buf);
                break;
            case RequestCallStack:
                RecvCallStack(&buf);
                break;
            case RequestVariables:
                recvRequestVariables(&buf);
                break;
            case RequestEvaluate:
                recvRequestEvaluate(&buf);
                break;
            case Disconnect:
                recvDisconnect(&buf);
                break;
            case ClearBreakpoints:
                recvClearBreakpoints(&buf);
                break;
            case SetBreakpoint:
                recvBreakpoint(&buf);
                break;
            case StopDebugging:
                recvStopDebugging(&buf);
                break;
            case RequestSetVariable:
                recvRequestSetVariable(&buf);
                break;
            }
        }
    }

    void sendData(const char* data, size_t length)
    {
        asio::async_write(*socket, asio::buffer(data, length),
            [](const asio::error_code& error, std::size_t /*bytes_transferred*/) {
                if (error) {
                    fmt::print(stderr, "Send error: {}\n", error.message());
                }
            });
    }
};

std::vector<std::unique_ptr<DebuggerClient>> clients;
std::mutex clientsMutex;

void addClientID(std::shared_ptr<tcp::socket> socket)
{
    std::lock_guard<std::mutex> lock(clientsMutex);
    try {
        if (auto it = std::find_if(clients.begin(), clients.end(),
            [&socket](const auto& client) {
                return client->socket == socket;
            });
            it == clients.end()) {
            clients.push_back(std::make_unique<DebuggerClient>(socket));
            clients.back()->AskFile();
        }
    }
    catch (const std::exception& e) {
        fmt::print(stderr, "Exception during client addition: {}\n", e.what());
    }
}

void removeClientID(std::shared_ptr<tcp::socket> socket)
{
    std::lock_guard<std::mutex> lock(clientsMutex);
    if (auto it = std::find_if(clients.begin(), clients.end(),
        [&socket](const auto& client) {
            return client->socket == socket;
        });
        it != clients.end()) {
        clients.erase(it);
    }
}

void debugThread()
{
    try {
        asio::io_context io_context;
        tcp::acceptor acceptor(io_context, tcp::endpoint(tcp::v4(), SM_Debugger_port()));

        fmt::print(stderr, "Debugger listening on port {}\n", SM_Debugger_port());

        std::atomic<bool> running{ true };

        std::function<void()> acceptConnection;
        acceptConnection = [&] {
            auto socket = std::make_shared<tcp::socket>(io_context);
            acceptor.async_accept(*socket, [&, socket](const asio::error_code& error) {
                if (!error) {
                    try {
                        socket->set_option(tcp::no_delay(true));
                        addClientID(socket);

                        auto buffer = std::make_shared<std::vector<char>>(1024);

                        // Define readHandler type explicitly before using it
                        std::function<void(const asio::error_code&, std::size_t)> readHandler;
                        readHandler = [&, socket, buffer](const asio::error_code& ec, std::size_t length) {
                            if (!ec) {
                                std::lock_guard<std::mutex> lock(clientsMutex);
                                for (auto& client : clients) {
                                    if (client->socket == socket) {
                                        try {
                                            client->RecvCmd(buffer->data(), length);
                                        }
                                        catch (const std::exception& e) {
                                            fmt::print(stderr, "Error processing command: {}\n", e.what());
                                        }
                                        break;
                                    }
                                }

                                socket->async_read_some(asio::buffer(*buffer), readHandler);
                            }
                            else {
                                // Inline removeClientID functionality
                                std::lock_guard<std::mutex> lock(clientsMutex);
                                if (auto it = std::find_if(clients.begin(), clients.end(),
                                    [&socket](const auto& client) {
                                        return client->socket == socket;
                                    });
                                    it != clients.end()) {
                                    clients.erase(it);
                                }
                            }
                            };

                        socket->async_read_some(asio::buffer(*buffer), readHandler);
                    }
                    catch (const std::exception& e) {
                        fmt::print(stderr, "Connection error: {}\n", e.what());
                    }
                }
                else {
                    fmt::print(stderr, "Accept error: {}\n", error.message());
                }

                if (running) {
                    acceptConnection();
                }
                });
            };

        acceptConnection();

        while (running) {
            try {
                io_context.run();
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            catch (const std::exception& e) {
                fmt::print(stderr, "Error in main loop: {}\n", e.what());
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }
        }
    }
    catch (const std::exception& e) {
        fmt::print(stderr, "Fatal error in debug thread: {}\n", e.what());
    }
}

void DebugReport::OnDebugSpew(const char* msg, ...)
{
    va_list ap;
    char buffer[512];

    va_start(ap, msg);
    ke::SafeVsprintf(buffer, sizeof(buffer), msg, ap);
    va_end(ap);
    original->OnDebugSpew(buffer);
}

void DebugReport::ReportError(const IErrorReport& report, IFrameIterator& iter)
{
    if (!clients.empty()) {
        auto plugin = report.Context();
        if (plugin) {
            bool found = false;
            for (auto& client : clients) {
                if (client && client->context_ == iter.Context()) {
                    found = true;
                    client->ReportError(report, iter);
                    break;
                }
            }

            if (!found) {
                for (int i = 0; i < report.Context()->GetRuntime()->GetDebugInfo()->NumFiles(); i++) {
                    auto filename = std::string(report.Context()->GetRuntime()->GetDebugInfo()->GetFileName(i));
                    auto current_file = std::filesystem::path(filename).filename().string();
                    lowercase(current_file);

                    for (auto& client : clients) {
                        if (client->files.find(current_file) != client->files.end()) {
                            client->ReportError(report, iter);
                        }
                    }
                }
            }
        }
    }

    if (DEBUG == 1) {
        fmt::print("VSCode extension request: {}\n", report.Message());
    }

    original->ReportError(report, iter);
}

void DebugHandler(SourcePawn::IPluginContext* IPlugin, sp_debug_break_info_t& BreakInfo, const SourcePawn::IErrorReport* IErrorReport)
{
    if (!IPlugin->IsDebugging())
        return;

    if (!clients.empty()) {
        for (auto it = clients.begin(); it != clients.end(); ++it) {
            const auto& client = *it;
            if (client && client->context_ == IPlugin) {
                try {
                    client->DebugHook(IPlugin, BreakInfo);
                }
                catch (DebuggerClient::debugger_stopped& ex) {
                    return;
                }
            }
        }

        for (int i = 0; i < IPlugin->GetRuntime()->GetDebugInfo()->NumFiles(); i++) {
            auto filename = IPlugin->GetRuntime()->GetDebugInfo()->GetFileName(i);
            auto current_file = std::filesystem::path(filename).filename().string();
            lowercase(current_file);

            for (auto it = clients.begin(); it != clients.end(); ++it) {
                const auto& client = *it;
                if (client->files.find(current_file) != client->files.end()) {
                    try {
                        client->DebugHook(IPlugin, BreakInfo);
                    }
                    catch (DebuggerClient::debugger_stopped& ex) {
                        return;
                    }
                }
            }
        }
    }
}