#pragma once

#include <smx/smx-legacy-debuginfo.h>
#include "smx-v1-debug-symbols.h"
#include <smx/smx-typeinfo.h>
#include <functional>


namespace sp {
class SmxV1Image;

class Symbol
{
  public:
    enum { VAR_PACKED, VAR_UNPACKED, VAR_RTTI };
    Symbol(sp_fdbg_symbol_t* sym, SmxV1Image* image)
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
     , unpacked_sym_(nullptr) {
    }

    Symbol(sp_u_fdbg_symbol_t* sym, SmxV1Image* image)
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
     , unpacked_sym_(sym) {
    }

    Symbol(smx_rtti_debug_var* sym, SmxV1Image* image)
     : addr_(sym->address)
     , codestart_(sym->code_start)
     , codeend_(sym->code_end)
     , name_(sym->name)
     , type_(VAR_RTTI)
     , sym_(nullptr)
     , unpacked_sym_(nullptr)
     , rtti_sym(sym) {
        dimcount_ = 0;
        enum {
            DISP_DEFAULT = 0x10,
            DISP_STRING = 0x20,
            DISP_BIN = 0x30, /* ??? not implemented */
            DISP_HEX = 0x40,
            DISP_BOOL = 0x50,
            DISP_FIXED = 0x60,
            DISP_FLOAT = 0x70
        };

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
    Symbol(Symbol* sym)
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

    const int32_t addr() const {
        return addr_;
    }
    const int16_t tagid() const {
        return tagid_;
    }
    const uint32_t codestart() const {
        return codestart_;
    }
    const uint32_t codeend() const {
        return codeend_;
    }
    const uint8_t ident() const {
        return ident_;
    }
    const uint8_t vclass() const {
        return vclass_;
    }
    const uint16_t dimcount() const {
        return dimcount_;
    }
    const uint32_t name() const {
        return name_;
    }
    void setVClass(uint8_t vclass) {
        vclass_ = vclass;
        if (sym_)
            sym_->vclass = vclass;
        else if (unpacked_sym_)
            unpacked_sym_->vclass = vclass;
        else
            rtti_sym->vclass = vclass;
    }
    const bool packed() const {
        return sym_ != nullptr;
    }
    const uint8_t type() const {
        return type_;
    }
    const smx_rtti_debug_var* rtti() const {
        return rtti_sym;
    }
    const void* sym() const {
        if (sym_) {
            return sym_;
        }
        if (sym_) {
            return unpacked_sym_;
        }
        if (rtti_sym) {
            return rtti_sym;
        }
    }

  private:
    int32_t addr_;       /**< Address rel to DAT or stack frame */
    int16_t tagid_;      /**< Tag id */
    uint32_t codestart_; /**< Start scope validity in code */
    uint32_t codeend_;   /**< End scope validity in code */
    uint8_t ident_;      /**< Variable type */
    uint8_t vclass_;     /**< Scope class (local vs global) */
    uint16_t dimcount_;  /**< Dimension count (for arrays) */
    uint32_t name_;      /**< Offset into debug nametable */
    uint8_t type_;

    sp_fdbg_symbol_t* sym_;
    sp_u_fdbg_symbol_t* unpacked_sym_;
    smx_rtti_debug_var* rtti_sym;
};

class ArrayDim
{
  public:
    ArrayDim(sp_fdbg_arraydim_t* dim)
     : tagid_(dim->tagid)
     , size_(dim->size) {
    }

    ArrayDim(sp_u_fdbg_arraydim_t* dim)
     : tagid_(dim->tagid)
     , size_(dim->size) {
    }
    ArrayDim(uint32_t size)
     : size_(size) {
    }

    int16_t tagid() {
        return tagid_;
    }
    uint32_t size() {
        return size_;
    }

  private:
    int16_t tagid_; /**< Tag id */
    uint32_t size_; /**< Size of dimension */
};

class SymbolIterator
{
  public:
    SymbolIterator(uint8_t* start, uint32_t debug_symbols_section_size, int type, SmxV1Image* image)
     : cursor_(start)
     , type_(type)
     , image_(image) {
        index_ = 0;
        cursor_end_ = cursor_ + debug_symbols_section_size;
    }

    bool Done();
    Symbol* Next();

  private:
    uint8_t* cursor_;
    uint8_t* cursor_end_;
    uint32_t index_;
    int type_;
    SmxV1Image* image_;
};
} // namespace sp