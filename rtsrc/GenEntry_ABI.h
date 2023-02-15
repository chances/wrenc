//
// Created by Campbell on 15/02/2023.
//

#pragma once

#include "common/common.h"

#ifndef GEN_ENTRY_ABI
#define GEN_ENTRY_ABI DLL_EXPORT
#endif

class ClosureSpec;
class RtModule;
class ObjFn;
class UpvalueStorage;

// These are the functions in question
// NOLINTBEGIN(readability-identifier-naming)
extern "C" {
GEN_ENTRY_ABI void *wren_virtual_method_lookup(Value receiver, uint64_t signature);
GEN_ENTRY_ABI void *wren_super_method_lookup(Value receiver, Value thisClassType, uint64_t signature, bool isStatic);
GEN_ENTRY_ABI Value wren_init_string_literal(void *getGlobalsFunc, const char *literal, int length);
GEN_ENTRY_ABI void wren_register_signatures_table(const char *signatures);
GEN_ENTRY_ABI Value wren_init_class(void *getGlobalsFunc, const char *name, uint8_t *dataBlock, Value parentClassValue);
GEN_ENTRY_ABI Value wren_alloc_obj(Value classVar);
GEN_ENTRY_ABI Value wren_alloc_foreign_obj(Value classVar, Value *arguments, int count);
GEN_ENTRY_ABI int wren_class_get_field_offset(Value classVar);
GEN_ENTRY_ABI ClosureSpec *wren_register_closure(void *specData);
GEN_ENTRY_ABI Value wren_create_closure(ClosureSpec *spec, void *stack, void *upvalueTable, ObjFn **listHead);
GEN_ENTRY_ABI Value **wren_get_closure_upvalue_pack(Value closure);
GEN_ENTRY_ABI UpvalueStorage *wren_alloc_upvalue_storage(int numClosures);
GEN_ENTRY_ABI void wren_unref_upvalue_storage(UpvalueStorage *storage);
GEN_ENTRY_ABI Value wren_get_bool_value(bool value);
GEN_ENTRY_ABI Value wren_get_core_class_value(const char *name);
GEN_ENTRY_ABI RtModule *wren_import_module(const char *name, void *getGlobalsFunc);
GEN_ENTRY_ABI Value wren_get_module_global(RtModule *mod, const char *name);
GEN_ENTRY_ABI Value wren_call_foreign_method(void **cacheVar, Value *args, int argsLen, Value classObj, void *funcPtr);
}
// NOLINTEND(readability-identifier-naming)
