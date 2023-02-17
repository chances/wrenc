//
// Used to copy out common code for the standalone main and library stubs.
//
// Created by Campbell on 17/02/2023.
//

#pragma once

#ifdef _WIN32
#define GEN_ENTRY_ABI __declspec(dllimport)
#endif

#include "GenEntry_ABI.h"

#ifdef _WIN32

extern "C" {
// This is exported by the random module built into the runtime
GEN_ENTRY_ABI void *random_get_globals();
};

// Force the compiler to import all the symbols the generated Wren
// modules need, as tinylink can't generate imports itself - it can
// merely use the addresses of those we've already caused.
void *linkerFuncPtrs[] = {
    (void *)wren_virtual_method_lookup,
    (void *)wren_super_method_lookup,
    (void *)wren_init_string_literal,
    (void *)wren_register_signatures_table,
    (void *)wren_init_class,
    (void *)wren_alloc_obj,
    (void *)wren_alloc_foreign_obj,
    (void *)wren_class_get_field_offset,
    (void *)wren_register_closure,
    (void *)wren_create_closure,
    (void *)wren_get_closure_upvalue_pack,
    (void *)wren_alloc_upvalue_storage,
    (void *)wren_unref_upvalue_storage,
    (void *)wren_get_bool_value,
    (void *)wren_get_core_class_value,
    (void *)wren_import_module,
    (void *)wren_get_module_global,
    (void *)wren_call_foreign_method,

    // We also need any bundled Wren modules
    (void *)random_get_globals,
};

#endif
