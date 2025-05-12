#pragma once

#include <smx/smx-legacy-debuginfo.h>
#include "smx-v1-debug-symbols.h"
#include <smx/smx-typeinfo.h>
#include <functional>
#include <memory>
#include <vector>


namespace sp {
class SmxV1Image;
struct smx_rtti_debug_var;
struct smx_rtti_table_header;

class Symbol
{
  public:
    enum { VAR_PACKED, VAR_UNPACKED, VAR_RTTI };
    Symbol(sp_fdbg_symbol_t* sym, SmxV1Image* image);
    Symbol(sp_u_fdbg_symbol_t* sym, SmxV1Image* image);
    Symbol(smx_rtti_debug_var* sym, SmxV1Image* image);
    Symbol(Symbol* sym);

    const int32_t addr() const;
    const int16_t tagid() const;
    const uint32_t codestart() const;
    const uint32_t codeend() const;
    const uint8_t ident() const;
    const uint8_t vclass() const;
    const uint16_t dimcount() const;
    const uint32_t name() const;
    void setVClass(uint8_t vclass);
    const bool packed() const;
    const uint8_t type() const;
    const smx_rtti_debug_var* rtti() const;
    const void* sym() const;

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
    ArrayDim(sp_fdbg_arraydim_t* dim);
    ArrayDim(sp_u_fdbg_arraydim_t* dim);
    ArrayDim(uint32_t size);

    int16_t tagid();
    uint32_t size();

  private:
    int16_t tagid_; /**< Tag id */
    uint32_t size_; /**< Size of dimension */
};

class SymbolIterator
{
  public:
    SymbolIterator(uint8_t* start, uint32_t debug_symbols_section_size, int type, SmxV1Image* image);

    bool Done();
    Symbol* Next();

  private:
    uint8_t* cursor_;
    uint8_t* cursor_end_;
    uint32_t index_;
    int type_;
    SmxV1Image* image_;
};

// Function declarations
SymbolIterator symboliterator(SmxV1Image* image, bool global);
bool GetVariable(SmxV1Image* image, const char* symname, uint32_t scopeaddr, std::unique_ptr<Symbol>& sym);
const char* GetTagName(SmxV1Image* image, int16_t tag_id);
std::vector<void*> getEnumFields(SmxV1Image* image, uint32_t enum_id);
std::vector<void*> getTypeFields(SmxV1Image* image, uint32_t type_id);
std::vector<ArrayDim*>* GetArrayDimensions(SmxV1Image* image, Symbol* sym);
const char* GetDebugName(SmxV1Image* image, uint32_t nameoffs);
void* rttidata(SmxV1Image* image);
size_t getTypeFromTypeId(uint32_t typeId);
void* typeFromTypeId(SmxV1Image* image, uint32_t type_id);
char* get_string(SourcePawn::IPluginContext* context, cell_t frm, Symbol* sym);
int get_symbolvalue(SourcePawn::IPluginContext* context, cell_t frm, const Symbol* sym, int index, cell_t* value);
int set_symbolvalue(SourcePawn::IPluginContext* context, cell_t frm, const Symbol* sym, int index, cell_t value);
void printvalue(long value, int disptype, std::string& out_value, std::string& out_type);
bool SetSymbolString(SourcePawn::IPluginContext* context, cell_t frm, const Symbol* sym, char* str);

template <typename T>
inline const T* getRttiRow(const smx_rtti_table_header* header, size_t index);

} // namespace sp