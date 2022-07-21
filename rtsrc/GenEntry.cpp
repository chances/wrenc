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
Value wren_sys_var_Object = NULL_VAL; // NOLINT(readability-identifier-naming)
Value wren_sys_var_Class = NULL_VAL;  // NOLINT(readability-identifier-naming)
Value wren_sys_var_System = NULL_VAL; // NOLINT(readability-identifier-naming)

Value wren_virtual_dispatch(Value receiver, const char *signature); // NOLINT(readability-identifier-naming)
}

Value wren_virtual_dispatch(Value receiver, const char *signature) {
	if (!is_object(receiver)) {
		// This does have to be implemented, look up the appropriate Num or Bool or whatever class
		printf("TODO - call on non-object with signature %s\n", signature);
		abort();
	}

	Obj *object = (Obj *)get_object_value(receiver);

	printf("On receiver of type %s invoke %s\n", object->type->name.c_str(), signature);

	return 0;
}

void setupGenEntry() {
	wren_sys_var_Object = CoreClasses::Instance()->Object().ToValue();
	wren_sys_var_Class = CoreClasses::Instance()->RootClass().ToValue();
	wren_sys_var_System = CoreClasses::Instance()->System().ToValue();
}
