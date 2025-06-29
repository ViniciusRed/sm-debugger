#include "helper.h"
#include "smx-v1-image.h"
#include <sp_vm_types.h>
#include <sp_vm_api.h>
#include <assert.h>
#include <string>
#include <functional>
#include <memory>
#include <vector>

namespace sp {
  
      // RTTI constants and structures 
    namespace cb {
        enum LayoutCode : uint8_t {
            // kBool,
            // kInt32,
            // kFloat32,
            // kChar8,
            // kAny,
            // kTopFunction,
            // kFunction,
            // kArray,
            // kFixedArray,
            // kEnumStruct,
            kStruct,
            kMethodmap,
            // kEnum,
            // kTypedef,
            // kClassdef,
            kMax
        };
    }

    enum {
        // kTypeId_Inline = 0,
        // kTypeId_Complex,
        kTypeId_Complex2,
        kTypeId_Bool,
        kTypeId_Int32,
        kTypeId_Float32,
        kTypeId_Char8,
        kTypeId_Any,
    };

    enum {
        DISP_DEFAULT = 0x10,
        DISP_STRING = 0x20,
        DISP_BIN = 0x30, /* ??? not implemented */
        DISP_HEX = 0x40,
        DISP_BOOL = 0x50,
        DISP_FIXED = 0x60,
        DISP_FLOAT = 0x70
    };

    // struct smx_rtti_field {
    //     uint32_t name;
    //     uint32_t type_id;
    //     uint32_t flags;
    // };

    // struct smx_rtti_es_field {
    //     uint32_t name;
    //     uint32_t type_id;
    //     uint32_t offset;
    //     uint32_t flags;
    // };

    // struct smx_rtti_enum {
    //     uint32_t name;
    //     uint32_t flags;
    // };

    // struct smx_rtti_enumstruct {
    //     uint32_t name;
    //     uint32_t first_field;
    //     uint32_t size;
    //     uint32_t flags;
    // };

    // struct smx_rtti_classdef {
    //     uint32_t name;
    //     uint32_t first_field;
    //     uint32_t flags;
    // };

    // struct smx_rtti_debug_var {
    //     uint32_t name;
    //     uint32_t type_id;
    //     uint32_t code_start;
    //     uint32_t code_end;
    //     uint32_t address;
    //     uint8_t vclass;
    //     uint8_t padding[3];
    // };

    // struct smx_rtti_table_header {
    //     uint8_t magic[4];
    //     uint32_t version;
    //     uint32_t header_size;
    //     uint32_t row_size;
    //     uint32_t row_count;
    // };

    // Symbol class implementation
    Symbol::Symbol(sp_fdbg_symbol_t* sym, SmxV1Image* image)
        : addr_(sym->addr)
        , tagid_(sym->tagid)
        , codestart_(sym->codestart)
        , codeend_(sym->codeend)
        , ident_(sym->ident)
        , vclass_(sym->vclass)
        , dimcount_(sym->dimcount)
        , name_(sym->name)
        , sym_(sym)
        , type_(VAR_PACKED)
        , unpacked_sym_(nullptr)
        , rtti_sym(nullptr) {
    }

    Symbol::Symbol(sp_u_fdbg_symbol_t* sym, SmxV1Image* image)
        : addr_(sym->addr)
        , tagid_(sym->tagid)
        , codestart_(sym->codestart)
        , codeend_(sym->codeend)
        , ident_(sym->ident)
        , vclass_(sym->vclass)
        , dimcount_(sym->dimcount)
        , name_(sym->name)
        , type_(VAR_UNPACKED)
        , sym_(nullptr)
        , unpacked_sym_(sym)
        , rtti_sym(nullptr) {
    }

    Symbol::Symbol(smx_rtti_debug_var* sym, SmxV1Image* image)
        : addr_(sym->address)
        , codestart_(sym->code_start)
        , codeend_(sym->code_end)
        , name_(sym->name)
        , type_(VAR_RTTI)
        , sym_(nullptr)
        , unpacked_sym_(nullptr)
        , rtti_sym(sym) {
        dimcount_ = 0;
        tagid_ = 0;

        auto DecodeUint32 = [](unsigned char* bytes, int& offset) {
            uint32_t value = 0;
            int shift = 0;
            for (;;) {
                unsigned char b = bytes[offset++];
                value |= (uint32_t)(b & 0x7f) << shift;
                if ((b & 0x80) == 0)
                    break;
                shift += 7;
            }
            return (int)value;
            };

        std::function<void(unsigned char*, int&)> Decode;
        Decode = [this, DecodeUint32, &Decode](unsigned char* bytes, int& offset) {
            unsigned char b = bytes[offset++];
            switch (b) {
            case cb::kFixedArray: {
                ident_ = IDENT_ARRAY;
                DecodeUint32(bytes, offset);
                dimcount_++;
                Decode(bytes, offset);
                break;
            }
            }
            };

        vclass_ = sym->vclass;
        int kind = (sym->type_id) & 0xf;
        int payload = ((sym->type_id) >> 4) & 0xfffffff;
        if (kind == kTypeId_Inline) {
            unsigned char temp[4];
            temp[0] = (payload & 0xff);
            temp[1] = ((payload >> 8) & 0xff);
            temp[2] = ((payload >> 16) & 0xff);
            temp[3] = ((payload >> 24) & 0xff);
            int offset = 0;
            Decode(temp, offset);
        }
    }

    Symbol::Symbol(Symbol* sym)
        : addr_(sym->addr_)
        , tagid_(sym->tagid_)
        , codestart_(sym->codestart_)
        , codeend_(sym->codeend_)
        , ident_(sym->ident_)
        , vclass_(sym->vclass_)
        , dimcount_(sym->dimcount_)
        , name_(sym->name_)
        , sym_(sym->sym_)
        , type_(sym->type_)
        , rtti_sym(sym->rtti_sym)
        , unpacked_sym_(sym->unpacked_sym_) {
    }

    const int32_t Symbol::addr() const {
        return addr_;
    }

    const int16_t Symbol::tagid() const {
        return tagid_;
    }

    const uint32_t Symbol::codestart() const {
        return codestart_;
    }

    const uint32_t Symbol::codeend() const {
        return codeend_;
    }

    const uint8_t Symbol::ident() const {
        return ident_;
    }

    const uint8_t Symbol::vclass() const {
        return vclass_;
    }

    const uint16_t Symbol::dimcount() const {
        return dimcount_;
    }

    const uint32_t Symbol::name() const {
        return name_;
    }

    void Symbol::setVClass(uint8_t vclass) {
        vclass_ = vclass;
        if (sym_)
            sym_->vclass = vclass;
        else if (unpacked_sym_)
            unpacked_sym_->vclass = vclass;
        else
            rtti_sym->vclass = vclass;
    }

    const bool Symbol::packed() const {
        return sym_ != nullptr;
    }

    const uint8_t Symbol::type() const {
        return type_;
    }

    const smx_rtti_debug_var* Symbol::rtti() const {
        return rtti_sym;
    }

    const void* Symbol::sym() const {
        if (sym_) {
            return sym_;
        }
        if (unpacked_sym_) {
            return unpacked_sym_;
        }
        if (rtti_sym) {
            return rtti_sym;
        }
        return nullptr;
    }

    // ArrayDim class implementation
    ArrayDim::ArrayDim(sp_fdbg_arraydim_t* dim)
        : tagid_(dim->tagid)
        , size_(dim->size) {
    }

    ArrayDim::ArrayDim(sp_u_fdbg_arraydim_t* dim)
        : tagid_(dim->tagid)
        , size_(dim->size) {
    }

    ArrayDim::ArrayDim(uint32_t size)
        : tagid_(0)
        , size_(size) {
    }

    int16_t ArrayDim::tagid() {
        return tagid_;
    }

    uint32_t ArrayDim::size() {
        return size_;
    }

    // SymbolIterator class implementation
    SymbolIterator::SymbolIterator(uint8_t* start, uint32_t debug_symbols_section_size, int type, SmxV1Image* image)
        : cursor_(start)
        , type_(type)
        , image_(image) {
        index_ = 0;
        cursor_end_ = cursor_ + debug_symbols_section_size;
    }

    // Implementation of SymbolIterator methods
    bool SymbolIterator::Done() {
        if (type_ == 1) {
            return cursor_ + sizeof(sp_fdbg_symbol_t) > cursor_end_;
        }
        else if (type_ == 0) {
            return cursor_ + sizeof(sp_u_fdbg_symbol_t) > cursor_end_;
        }
        else if (type_ == 2) {
            // Default to using a reasonable limit since we can't access private members
            return index_ >= 1000;
        }
        else {
            // Default to using a reasonable limit since we can't access private members
            return index_ >= 1000;
        }
    }

    Symbol* SymbolIterator::Next() {
        if (type_ == 1) {
            sp_fdbg_symbol_t* sym = reinterpret_cast<sp_fdbg_symbol_t*>(cursor_);
            if (sym->dimcount > 0)
                cursor_ += sizeof(sp_fdbg_arraydim_t) * sym->dimcount;
            cursor_ += sizeof(sp_fdbg_symbol_t);

            return new Symbol(sym, nullptr);
        }
        else if (type_ == 0) {
            sp_u_fdbg_symbol_t* sym = reinterpret_cast<sp_u_fdbg_symbol_t*>(cursor_);
            if (sym->dimcount > 0)
                cursor_ += sizeof(sp_u_fdbg_arraydim_t) * sym->dimcount;
            cursor_ += sizeof(sp_u_fdbg_symbol_t);

            return new Symbol(sym, nullptr);
        }
        else {
            // Create a dummy symbol for testing since we can't access private members
            smx_rtti_debug_var* sym = new smx_rtti_debug_var();
            sym->name = 0;
            sym->type_id = 0;
            sym->code_start = 0;
            sym->code_end = 0;
            sym->address = 0;
            sym->vclass = 0;

            index_++;
            return new Symbol(sym, image_);
        }
    }

    // Template function implementation
    template <typename T>
    inline const T* getRttiRow(const smx_rtti_table_header* header, size_t index) {
        const uint8_t* base = reinterpret_cast<const uint8_t*>(header) + header->header_size;
        const uint8_t* row = base + (index * header->row_size);
        return reinterpret_cast<const T*>(row);
    }

    // Standalone function implementations from smx-v1-image.cpp

    // Nova versão: o debugger deve passar os ponteiros/tamanhos explicitamente
    SymbolIterator symboliterator(
        SmxV1Image* image,
        uint8_t* packed_syms, uint32_t packed_syms_size,
        uint8_t* unpacked_syms, uint32_t unpacked_syms_size,
        uint8_t* rtti_locals, uint32_t rtti_locals_size,
        uint8_t* rtti_globals, uint32_t rtti_globals_size,
        bool global
    ) {
        if (!image)
            return SymbolIterator(nullptr, 0, 0, image);

        if (packed_syms && packed_syms_size && !global)
            return SymbolIterator(packed_syms, packed_syms_size, 1, image);
        if (unpacked_syms && unpacked_syms_size && !global)
            return SymbolIterator(unpacked_syms, unpacked_syms_size, 0, image);
        if (!global && rtti_locals && rtti_locals_size)
            return SymbolIterator(rtti_locals, rtti_locals_size, 2, image);
        if (global && rtti_globals && rtti_globals_size)
            return SymbolIterator(rtti_globals, rtti_globals_size, 3, image);
        return SymbolIterator(nullptr, 0, 0, image);
    }

    // Gets a variable by name from the SmxV1Image
    bool GetVariable(SmxV1Image* image, const char* symname, uint32_t scopeaddr, std::unique_ptr<Symbol>& sym) {
        sym = nullptr;

        // Ponteiros públicos para RTTI locals/globals (ajuste conforme métodos públicos disponíveis)
        const smx_rtti_table_header* rtti_locals = nullptr;
        const smx_rtti_table_header* rtti_globals = nullptr;
        // TODO: Se existirem métodos públicos, obtenha os ponteiros aqui
        SymbolIterator iter = symboliterator(
            image,
            nullptr, 0, // packed_syms
            nullptr, 0, // unpacked_syms
            (uint8_t*)rtti_locals, rtti_locals ? (rtti_locals->row_count * rtti_locals->row_size) : 0,
            (uint8_t*)rtti_globals, rtti_globals ? (rtti_globals->row_count * rtti_globals->row_size) : 0,
            false
        );
        while (!iter.Done()) {
            auto symbol = iter.Next();
            // Match based on name and scope
            const char* name = GetDebugName(image, symbol->name());
            if (symbol->codestart() <= scopeaddr &&
                symbol->codeend() >= scopeaddr &&
                name &&
                strcmp(name, symname) == 0) {
                sym = std::unique_ptr<Symbol>(symbol);
                return true;
            }
            delete symbol;
        }

        // Try global variables
        iter = symboliterator(
            image,
            nullptr, 0, // packed_syms
            nullptr, 0, // unpacked_syms
            (uint8_t*)rtti_locals, rtti_locals ? (rtti_locals->row_count * rtti_locals->row_size) : 0,
            (uint8_t*)rtti_globals, rtti_globals ? (rtti_globals->row_count * rtti_globals->row_size) : 0,
            true
        );
        while (!iter.Done()) {
            auto symbol = iter.Next();
            // Match based on name only for globals
            const char* name = GetDebugName(image, symbol->name());
            if (name && strcmp(name, symname) == 0) {
                sym = std::unique_ptr<Symbol>(symbol);
                return true;
            }
            delete symbol;
        }

        return false;
    }

    // Gets a tag name from the SmxV1Image (compatível com SourceMod)
    const char* GetTagName(SmxV1Image* image, int16_t tag_id) {
        if (!image)
            return nullptr;
        auto tag = image->GetTagById((uint32_t)tag_id);
        if (!tag)
            return nullptr;
        return image->GetDebugName(tag->name);
    }

    // Gets enumstruct fields from a RTTI table (sem acessar membros privados)
    template <typename T>
    inline const T* localGetRttiRow(const smx_rtti_table_header* header, size_t index) {
        const uint8_t* base = reinterpret_cast<const uint8_t*>(header) + header->header_size;
        return reinterpret_cast<const T*>(base + header->row_size * index);
    }

    std::vector<void*> getEnumFields(const smx_rtti_table_header* enumstructs, const smx_rtti_table_header* enumstruct_fields, uint32_t index) {
        std::vector<void*> ret;
        if (!enumstructs || !enumstruct_fields)
            return ret;
        const smx_rtti_enumstruct* enumstruct = localGetRttiRow<smx_rtti_enumstruct>(enumstructs, index);
        uint32_t first = enumstruct->first_field;
        uint32_t last = (index + 1 < enumstructs->row_count)
            ? localGetRttiRow<smx_rtti_enumstruct>(enumstructs, index + 1)->first_field
            : enumstruct_fields->row_count;
        for (uint32_t j = first; j < last; j++) {
            const smx_rtti_es_field* field = localGetRttiRow<smx_rtti_es_field>(enumstruct_fields, j);
            if (field)
                ret.push_back((void*)field);
        }
        return ret;
    }

    // Gets type fields from a RTTI table (sem acessar membros privados)
    std::vector<void*> getTypeFields(const smx_rtti_table_header* classdefs, const smx_rtti_table_header* fields, uint32_t index) {
        std::vector<void*> ret;
        if (!classdefs || !fields)
            return ret;
        const smx_rtti_classdef* classdef = localGetRttiRow<smx_rtti_classdef>(classdefs, index);
        uint32_t first = classdef->first_field;
        uint32_t last = (index + 1 < classdefs->row_count)
            ? localGetRttiRow<smx_rtti_classdef>(classdefs, index + 1)->first_field
            : fields->row_count;
        for (uint32_t j = first; j < last; j++) {
            const smx_rtti_field* field = localGetRttiRow<smx_rtti_field>(fields, j);
            if (field)
                ret.push_back((void*)field);
        }
        return ret;
    }

    // Gets array dimensions from a symbol
    std::vector<ArrayDim*>* GetArrayDimensions(SmxV1Image* image, Symbol* sym) {
        if (sym->ident() != IDENT_ARRAY && sym->ident() != IDENT_REFARRAY)
            return nullptr;

        assert(sym->dimcount() > 0); // array must have at least one dimension

        // Find the end of the symbol name
        const char* ptr = (const char*)sym->sym();
        auto type = sym->type();
        if (type == Symbol::VAR_PACKED) {
            ptr += sizeof(sp_fdbg_symbol_t);
        }
        else if (type == Symbol::VAR_UNPACKED) {
            ptr += sizeof(sp_u_fdbg_symbol_t);
        }

        if (type != Symbol::VAR_RTTI) {
            std::vector<ArrayDim*>* dims = new std::vector<ArrayDim*>();
            for (int i = 0; i < sym->dimcount(); i++) {
                if (sym->packed()) {
                    dims->push_back(new ArrayDim((sp_fdbg_arraydim_t*)ptr));
                    ptr += sizeof(sp_fdbg_arraydim_t);
                }
                else {
                    // There's a padding of 2 bytes before this short.
                    ptr += 2;
                    dims->push_back(new ArrayDim((sp_u_fdbg_arraydim_t*)ptr));
                    ptr += sizeof(sp_u_fdbg_arraydim_t);
                }
            }
            return dims;
        }
        else {
            std::vector<ArrayDim*>* dims = new std::vector<ArrayDim*>();
            auto sym_var = (smx_rtti_debug_var*)ptr;
            int kind = (sym_var->type_id) & 0xf;
            int payload = ((sym_var->type_id) >> 4) & 0xfffffff;
            auto DecodeUint32 = [](unsigned char* bytes, int& offset) {
                uint32_t value = 0;
                int shift = 0;
                for (;;) {
                    unsigned char b = bytes[offset++];
                    value |= (uint32_t)(b & 0x7f) << shift;
                    if ((b & 0x80) == 0)
                        break;
                    shift += 7;
                }
                return (int)value;
                };
            std::function<void(unsigned char*, int&)> Decode;
            Decode = [dims, DecodeUint32, &Decode](unsigned char* bytes, int& offset) {
                unsigned char b = bytes[offset++];
                switch (b) {
                case cb::kFixedArray: {
                    auto dimcount_ = DecodeUint32(bytes, offset);
                    dims->push_back(new ArrayDim(dimcount_));
                    Decode(bytes, offset);
                    break;
                }
                }
                };
            if (kind == kTypeId_Inline) {
                unsigned char temp[4];
                temp[0] = (payload & 0xff);
                temp[1] = ((payload >> 8) & 0xff);
                temp[2] = ((payload >> 16) & 0xff);
                temp[3] = ((payload >> 24) & 0xff);
                int offset = 0;
                Decode(temp, offset);
            }
            return dims;
        }
    }

    // Gets a debug name from the SmxV1Image
    const char* GetDebugName(SmxV1Image* image, uint32_t nameoffs) {
        if (!image)
            return nullptr;
        return image->GetDebugName(nameoffs);
    }

    // Gets RTTI data from the SmxV1Image
    void* rttidata(SmxV1Image* image) {
        // Return null since we can't access private members
        return nullptr;
    }

    // Gets a type from type ID in the SmxV1Image
    size_t getTypeFromTypeId(uint32_t typeId) {
        int kind = (typeId) & 0xf;
        int payload = ((typeId) >> 4) & 0xfffffff;
        auto DecodeUint32 = [](unsigned char* bytes, int& offset) {
            uint32_t value = 0;
            int shift = 0;
            for (;;) {
                unsigned char b = bytes[offset++];
                value |= (uint32_t)(b & 0x7f) << shift;
                if ((b & 0x80) == 0)
                    break;
                shift += 7;
            }
            return (int)value;
            };

        if (kind == kTypeId_Inline) {
            unsigned char temp[4];
            temp[0] = (payload & 0xff);
            temp[1] = ((payload >> 8) & 0xff);
            temp[2] = ((payload >> 16) & 0xff);
            temp[3] = ((payload >> 24) & 0xff);
            int offset = 0;
            char b = temp[offset++];
            if (b == cb::kConst) {
                b = temp[offset++];
            }
            return b;
        }
        return 0;
    }

    // Gets a type from type ID in the SmxV1Image
    void* typeFromTypeId(SmxV1Image* image, uint32_t type_id) {
        // Return null since we can't access private members
        return nullptr;
    }

    // Core helper functions for symbols and variables

    // Gets a string value from a symbol
    char* get_string(SourcePawn::IPluginContext* context, cell_t frm, Symbol* sym) {
        assert(sym->ident() == IDENT_ARRAY || sym->ident() == IDENT_REFARRAY);
        assert(sym->dimcount() == 1);

        // get the starting address and the length of the string
        cell_t* addr;
        cell_t base = sym->addr();
        if (sym->vclass() == 1 || sym->vclass() == 3) // local var or arg but not static
            base += frm; // addresses of local vars are relative to the frame
        if (sym->ident() == IDENT_REFARRAY) {
            context->LocalToPhysAddr(base, &addr);
            assert(addr != nullptr);
            base = *addr;
        }

        char* str;
        if (context->LocalToStringNULL(base, &str) != SP_ERROR_NONE)
            return nullptr;
        return str;
    }

    // Gets a value from a symbol
    int get_symbolvalue(SourcePawn::IPluginContext* context, cell_t frm, const Symbol* sym, int index, cell_t* value) {
        cell_t* vptr;
        cell_t base = sym->addr();
        if (sym->vclass() & 0x0f)
            base += frm; // addresses of local vars are relative to the frame

        // a reference
        if (sym->ident() == IDENT_REFERENCE || sym->ident() == IDENT_REFARRAY) {
            if (context->LocalToPhysAddr(base, &vptr) != SP_ERROR_NONE)
                return false;

            assert(vptr != nullptr);
            base = *vptr;
        }

        if (context->LocalToPhysAddr(base + index * sizeof(cell_t), &vptr) != SP_ERROR_NONE)
            return false;

        if (vptr != nullptr)
            *value = *vptr;
        return vptr != nullptr;
    }

    // Sets a value to a symbol
    int set_symbolvalue(SourcePawn::IPluginContext* context, cell_t frm, const Symbol* sym, int index, cell_t value) {
        cell_t* vptr;
        cell_t base = sym->addr();
        if (sym->vclass() & 0x0f)
            base += frm; // addresses of local vars are relative to the frame

        // a reference
        if (sym->ident() == IDENT_REFERENCE || sym->ident() == IDENT_REFARRAY) {
            context->LocalToPhysAddr(base, &vptr);
            assert(vptr != nullptr);
            base = *vptr;
        }

        context->LocalToPhysAddr(base + index * sizeof(cell_t), &vptr);
        assert(vptr != nullptr);
        *vptr = value;
        return true;
    }

    // Formats a value for display
    void printvalue(long value, int disptype, std::string& out_value, std::string& out_type) {
        char out[64];
        if (disptype == DISP_FLOAT) {
            out_type = "float";
            sprintf(out, "%f", sp_ctof(value));
        }
        else if (disptype == DISP_FIXED) {
            out_type = "fixed";
            long ipart = value / 1000;
            value -= 1000 * ipart;
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
            } /* switch */
        }
        else {
            out_type = "cell";
            sprintf(out, "%ld", value);
        } /* if */
        out_value += out;
    }
}