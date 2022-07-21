//
// Generated entrypoint functions
// These are functions that the generated code calls, with the exception of setupGenEntry().
//
// Created by znix on 21/07/22.
//

#include "GenEntry.h"
#include "CoreClasses.h"
#include "ObjClass.h"
#include "ObjSystem.h"
#include "common.h"

// These are the functions in question
extern "C" {
ObjClass *wren_sys_var_Object = nullptr; // NOLINT(readability-identifier-naming)
ObjClass *wren_sys_var_Class = nullptr;  // NOLINT(readability-identifier-naming)
ObjClass *wren_sys_var_System = nullptr; // NOLINT(readability-identifier-naming)

Value wren_virtual_dispatch(Value receiver, const char *signature); // NOLINT(readability-identifier-naming)
}

Value wren_virtual_dispatch(Value receiver, const char *signature) { return 0; }

void setupGenEntry() {
	wren_sys_var_Object = &CoreClasses::Instance()->Object();
	wren_sys_var_Class = &CoreClasses::Instance()->RootClass();
	wren_sys_var_System = &CoreClasses::Instance()->System();
}
