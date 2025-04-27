// vim: set sts=2 ts=8 sw=2 tw=99 et:
//
// Copyright (C) 2004-2015 AlliedModers LLC
//
// This file is part of SourcePawn. SourcePawn is licensed under the GNU
// General Public License, version 3.0 (GPL). If a copy of the GPL was not
// provided with this file, you can obtain it here:
//   http://www.gnu.org/licenses/gpl.html
//
#ifndef _include_sourcepawn_smx_parser_h_
#define _include_sourcepawn_smx_parser_h_

#include <amtl/am-string.h>
#include <amtl/am-vector.h>
#include <smx/smx-headers.h>
#include <smx/smx-legacy-debuginfo.h>
#include <smx/smx-typeinfo.h>
#include <smx/smx-v1.h>
#include <sp_vm_types.h>
#include <stdio.h>
#include <functional>
#include "file-utils.h"
#include "legacy-image.h"
#include "rtti.h"
#include "smx-v1-debug-symbols.h"

#include <memory>

namespace sp {

    using namespace debug;

    class SmxV1Image : public FileReader, public LegacyImage
    {
        friend SmxV1SymbolIterator;
        friend SmxV1LegacySymbolIterator;

    public:
        SmxV1Image(FILE* fp);
        SmxV1Image(uint8_t* addr, size_t length);
        SmxV1Image(uint8_t* addr, size_t length, void (*dtor)(uint8_t*));

        // This must be called to initialize the reader.
        bool validate();

        const sp_file_hdr_t* hdr() const {
            return hdr_;
        }

        const char* errorMessage() const {
            return error_.c_str();
        }

    public:
        Code DescribeCode() const override;
        Data DescribeData() const override;
        size_t NumNatives() const override;
        const char* GetNative(size_t index) const override;
        bool FindNative(const char* name, size_t* indexp) const override;
        size_t NumPublics() const override;
        void GetPublic(size_t index, uint32_t* offsetp, const char** namep) const override;
        bool FindPublic(const char* name, size_t* indexp) const override;
        size_t NumPubvars() const override;
        void GetPubvar(size_t index, uint32_t* offsetp, const char** namep) const override;
        bool FindPubvar(const char* name, size_t* indexp) const override;
        size_t HeapSize() const override;
        size_t ImageSize() const override;
        const char* LookupFile(uint32_t code_offset) const override;
        const char* LookupFunction(uint32_t code_offset) const override;
        bool LookupLine(uint32_t code_offset, uint32_t* line) const override;
        bool LookupFunctionAddress(const char* function, const char* file,
            ucell_t* addr) const override;
        bool LookupLineAddress(const uint32_t line, const char* file, ucell_t* addr) const override;
        size_t NumFiles() const override;
        const char* GetFileName(size_t index) const override;
        size_t NumFunctions() const override;
        const char* GetFunctionName(size_t index, const char** filename) const override;
        SourcePawn::IDebugSymbolIterator* SymbolIterator(ucell_t addr) override;
        const sp_file_tag_t* GetTagById(uint32_t tagid) const;
        const char* GetTagName(uint32_t tag);
        const char* GetName(uint32_t nameoffs) const;
        const char* GetDebugName(uint32_t nameoffs) const;
        bool HasRtti() const override;
        const smx_rtti_method* GetMethodRttiByOffset(uint32_t pcode_offset) const override;

        bool GetVariable(const char* symname, uint32_t scopeaddr, std::unique_ptr<sp::Symbol>& sym);

        const smx_rtti_classdef* GetRttiClassdef(uint32_t index) const;
        const smx_rtti_enum* GetRttiEnum(uint32_t index) const;
        const smx_rtti_enumstruct* GetRttiEnumStruct(uint32_t index) const;
        uint32_t GetRttiEnumStructLastFieldIndex(uint32_t index) const;
        const smx_rtti_es_field* GetRttiEnumStructField(uint32_t index) const;
        const smx_rtti_native* GetRttiNative(uint32_t index) const;
        const smx_rtti_typedef* GetRttiTypedef(uint32_t index) const;
        const smx_rtti_typeset* GetRttiTypeset(uint32_t index) const;

    private:
        SmxV1Image();

        struct Section {
            const char* name;
            uint32_t dataoffs;
            uint32_t size;
        };
        const Section* findSection(const char* name) const;

    public:
        template <typename T>
        class Blob
        {
        public:
            Blob()
                : header_(nullptr)
                , section_(nullptr)
                , blob_(nullptr)
                , length_(0)
                , features_(0) {
            }
            Blob(const Section* header, const T* section, const uint8_t* blob, size_t length,
                uint32_t features)
                : header_(header)
                , section_(section)
                , blob_(blob)
                , length_(length)
                , features_(features) {
            }

            size_t size() const {
                return section_->size;
            }
            const T* operator->() const {
                return section_;
            }
            const uint8_t* blob() const {
                return blob_;
            }
            size_t length() const {
                return length_;
            }
            bool exists() const {
                return !!header_;
            }
            uint32_t features() const {
                return features_;
            }
            const Section* header() const {
                return header_;
            }

        private:
            const Section* header_;
            const T* section_;
            const uint8_t* blob_;
            size_t length_;
            uint32_t features_;
        };

        template <typename T>
        class List
        {
        public:
            List()
                : section_(nullptr)
                , length_(0) {
            }
            List(const T* section, size_t length)
                : section_(section)
                , length_(length) {
            }

            size_t length() const {
                return length_;
            }
            const T& operator[](size_t index) const {
                assert(index < length());
                return section_[index];
            }
            bool exists() const {
                return !!section_;
            }

        private:
            const T* section_;
            size_t length_;
        };

    public:
        const Blob<sp_file_code_t>& code() const {
            return code_;
        }
        const Blob<sp_file_data_t>& data() const {
            return data_;
        }
        const List<sp_file_publics_t>& publics() const {
            return publics_;
        }
        const List<sp_file_natives_t>& natives() const {
            return natives_;
        }
        const List<sp_file_pubvars_t>& pubvars() const {
            return pubvars_;
        }
        const RttiData* rttidata() const {
            return rtti_data_.get();
        }

        std::vector<smx_rtti_es_field*> getEnumFields(uint32_t index);
        std::vector<smx_rtti_field*> getTypeFields(uint32_t index);
        std::vector<sp::ArrayDim*>* GetArrayDimensions(const sp::Symbol* sym);
        sp::SymbolIterator symboliterator(bool global = false);

    protected:
        bool error(const char* msg) {
            error_ = msg;
            return false;
        }
        bool validateName(size_t offset) const;
        bool validateSection(const Section* section) const;
        bool validateRttiHeader(const Section* section) const;
        bool validateCode();
        bool validateData();
        bool validatePublics();
        bool validatePubvars();
        bool validateNatives();
        bool validateRtti();
        bool validateRttiClassdefs();
        bool validateRttiEnums();
        bool validateRttiEnumStructs();
        bool validateRttiEnumStructField(const smx_rtti_enumstruct* enumstruct, uint32_t index);
        bool validateRttiField(uint32_t index);
        bool validateRttiMethods();
        bool validateRttiNatives();
        bool validateRttiTypedefs();
        bool validateRttiTypesets();
        bool validateDebugInfo();
        bool validateDebugVariables(const smx_rtti_table_header* rtti_table);
        bool validateDebugMethods();
        bool validateSymbolAddress(int32_t address, uint8_t vclass, const sp::debug::Rtti* rtti_type);
        bool validateDebugName(size_t offset);
        template <typename SymbolType, typename DimType>
        bool validateLegacyDebugSymbols();
        bool validateLegacySymbolAddress(int32_t address, uint8_t vclass, uint8_t ident);
        bool validateTags();
        bool validateTag(int16_t tagid);

    private:
        template <typename SymbolType, typename DimType>
        const char* lookupFunction(const SymbolType* syms, uint32_t addr) const;
        template <typename SymbolType, typename DimType>
        uint32_t getFunctionCount(const SymbolType* syms) const;
        template <typename SymbolType, typename DimType>
        const char* getFunctionName(const SymbolType* syms, const char** filename,
            uint32_t index) const;
        template <typename SymbolType, typename DimType>
        bool getFunctionAddress(const SymbolType* syms, const char* function, ucell_t* funcaddr,
            size_t index) const;

        const smx_rtti_table_header* findRttiSection(const char* name) const {
            const Section* section = findSection(name);
            if (!section)
                return nullptr;
            return reinterpret_cast<const smx_rtti_table_header*>(buffer() + section->dataoffs);
        }

        const smx_rtti_table_header* toRttiTable(const Section* section) const {
            return reinterpret_cast<const smx_rtti_table_header*>(buffer() + section->dataoffs);
        }

        template <typename T>
        const T* getRttiRow(const smx_rtti_table_header* header, size_t index) const {
            assert(index < header->row_count);
            const uint8_t* base = reinterpret_cast<const uint8_t*>(header) + header->header_size;
            return reinterpret_cast<const T*>(base + header->row_size * index);
        }

    private:
        sp_file_hdr_t* hdr_ = nullptr;
        std::string error_;
        const char* header_strings_ = nullptr;
        std::vector<Section> sections_;

        const Section* names_section_ = nullptr;
        const char* names_ = nullptr;

        Blob<sp_file_code_t> code_;
        Blob<sp_file_data_t> data_;
        List<sp_file_publics_t> publics_;
        List<sp_file_natives_t> natives_;
        List<sp_file_pubvars_t> pubvars_;
        List<sp_file_tag_t> tags_;

        const Section* debug_names_section_ = nullptr;
        const char* debug_names_ = nullptr;
        const sp_fdbg_info_t* debug_info_ = nullptr;
        List<sp_fdbg_file_t> debug_files_;
        List<sp_fdbg_line_t> debug_lines_;
        const Section* debug_symbols_section_ = nullptr;
        const sp_fdbg_symbol_t* debug_syms_ = nullptr;
        const sp_u_fdbg_symbol_t* debug_syms_unpacked_ = nullptr;
    public:
        std::unique_ptr<const RttiData> rtti_data_ = nullptr;
        const smx_rtti_table_header* rtti_classdefs_ = nullptr;
        const smx_rtti_table_header* rtti_enums_ = nullptr;
        const smx_rtti_table_header* rtti_enumstructs_ = nullptr;
        const smx_rtti_table_header* rtti_enumstruct_fields_ = nullptr;
        const smx_rtti_table_header* rtti_fields_ = nullptr;
        const smx_rtti_table_header* rtti_methods_ = nullptr;
        const smx_rtti_table_header* rtti_natives_ = nullptr;
        const smx_rtti_table_header* rtti_typedefs_ = nullptr;
        const smx_rtti_table_header* rtti_typesets_ = nullptr;
        const smx_rtti_table_header* rtti_dbg_globals_ = nullptr;
        const smx_rtti_table_header* rtti_dbg_methods_ = nullptr;
        const smx_rtti_table_header* rtti_dbg_locals_ = nullptr;
    };

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

        bool Done() {
            if (type_ == 1) {
                return cursor_ + sizeof(sp_fdbg_symbol_t) > cursor_end_;
            }
            else if (type_ == 0) {
                return cursor_ + sizeof(sp_u_fdbg_symbol_t) > cursor_end_;
            }
            else if (type_ == 2) {
                return index_ >= image_->rtti_dbg_locals_->row_count;
            }
            else {
                return index_ >= image_->rtti_dbg_globals_->row_count;
            }
        }

        Symbol* Next() {
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
                const smx_rtti_debug_var* sym = image_->getRttiRow<smx_rtti_debug_var>(
                    (type_ == 2) ? image_->rtti_dbg_locals_ : image_->rtti_dbg_globals_, index_);
                //smx_rtti_debug_var* sym = reinterpret_cast<smx_rtti_debug_var*>(cursor_);
                index_ += 1;
                return new Symbol((smx_rtti_debug_var*)sym, image_);
            }
        }

    private:
        uint8_t* cursor_;
        uint8_t* cursor_end_;
        uint32_t index_;
        int type_;
        SmxV1Image* image_;
    };

} // namespace sp

#endif // _include_sourcepawn_smx_parser_h_
