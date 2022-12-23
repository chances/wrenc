//
// Generated entrypoint functions
// These are functions that the generated code calls, with the exception of setupGenEntry().
//
// Created by znix on 21/07/22.
//

#include "GenEntry.h"
#include "CoreClasses.h"
#include "Errors.h"
#include "ObjBool.h"
#include "ObjClass.h"
#include "ObjFibre.h"
#include "ObjFn.h"
#include "ObjList.h"
#include "ObjManaged.h"
#include "ObjMap.h"
#include "ObjNull.h"
#include "ObjNum.h"
#include "ObjRange.h"
#include "ObjSequence.h"
#include "ObjString.h"
#include "ObjSystem.h"
#include "RtModule.h"
#include "WrenRuntime.h"
#include "common.h"
#include "common/ClassDescription.h"

#include <inttypes.h>
#include <string.h>
#include <vector>

#define EXPORT __attribute__((visibility("default")))

// These are the functions in question
// NOLINTBEGIN(readability-identifier-naming)
extern "C" {
EXPORT void *wren_virtual_method_lookup(Value receiver, uint64_t signature);
EXPORT void *wren_super_method_lookup(Value receiver, Value thisClassType, uint64_t signature, bool isStatic);
EXPORT Value wren_init_string_literal(const char *literal, int length);
EXPORT void wren_register_signatures_table(const char *signatures);
EXPORT Value wren_init_class(const char *name, uint8_t *dataBlock, Value parentClassValue);
EXPORT Value wren_alloc_obj(Value classVar);
EXPORT int wren_class_get_field_offset(Value classVar);
EXPORT ClosureSpec *wren_register_closure(void *specData);
EXPORT Value wren_create_closure(ClosureSpec *spec, void *stack, void *upvalueTable, ObjFn **listHead);
EXPORT Value **wren_get_closure_upvalue_pack(ObjFn *closure);
EXPORT ObjFn *wren_get_closure_chain_next(ObjFn *closure);
EXPORT void *wren_alloc_upvalue_storage(int numClosures);
EXPORT Value wren_get_bool_value(bool value);
EXPORT Value wren_get_core_class_value(const char *name);
EXPORT RtModule *wren_import_module(const char *name, void *getGlobalsFunc);
EXPORT Value wren_get_module_global(RtModule *mod, const char *name);
}
// NOLINTEND(readability-identifier-naming)

void *wren_virtual_method_lookup(Value receiver, uint64_t signature) {
	ObjClass *type;

	if (is_object(receiver)) {
		Obj *object = (Obj *)get_object_value(receiver);
		if (object) {
			type = object->type;
		} else {
			type = ObjNull::Class();
		}
	} else {
		// If it's not an object it must be a number, so say the receiver's type happens to be that
		type = ObjNumClass::Instance();
	}

	FunctionTable::Entry *func = type->LookupMethod(SignatureId{signature});

	if (!func) {
		std::string name = ObjClass::LookupSignatureFromId({signature}, true);
		errors::wrenAbort("%s does not implement '%s'.", type->name.c_str(), name.c_str());
	}

	// printf("On receiver of type %s invoke %lx func %p\n", object->type->name.c_str(), signature, func->func);

	return func->func;
}

void *wren_super_method_lookup(Value receiver, Value thisClass, uint64_t signature, bool isStatic) {
	if (!is_object(receiver)) {
		errors::wrenAbort("Cannot lookup super method on numbers - maybe this is memory corruption?\n");
	}

	Obj *obj = get_object_value(thisClass);
	ObjClass *cls = dynamic_cast<ObjManagedClass *>(obj);
	if (!cls) {
		errors::wrenAbort("Attempted to lookup super method on something other than a managed class.\n");
	}

	// Normally, calls to static methods get dispatched properly as the receiver is the ObjClass. However, since
	// we're doing our own lookup that has to be handled specially.
	if (isStatic) {
		cls = cls->type;
	}

	// We've been passed the class of the method that's making the call. It's trying to call a method
	// from it's parent, so here we use parentClass.
	FunctionTable::Entry *func = cls->parentClass->LookupMethod(SignatureId{signature});

	if (!func) {
		std::string name = ObjClass::LookupSignatureFromId({signature}, true);
		errors::wrenAbort("%s does not implement '%s'.", cls->parentClass->name.c_str(), name.c_str());
	}

	return func->func;
}

Value wren_init_string_literal(const char *literal, int length) {
	ObjString *str = WrenRuntime::Instance().New<ObjString>();
	str->m_value = std::string(literal, length);
	return encode_object(str);
}

void wren_register_signatures_table(const char *signatures) {
	while (true) {
		// If there's an empty string, that signifies the end of the table
		if (*signatures == 0)
			break;

		// Find and process this string
		std::string signature = signatures;
		signatures += signature.size() + 1; // +1 for trailing null

		// Looking up a signature is enough to register it
		ObjClass::FindSignatureId(signature);
	}
}

Value wren_init_class(const char *name, uint8_t *dataBlock, Value parentClassValue) {
	std::unique_ptr<ClassDescription> spec = std::make_unique<ClassDescription>();
	spec->Parse(dataBlock);

	if (!is_object(parentClassValue)) {
		errors::wrenAbort("Invalid non-object parent class for '%s': 0x%" PRIx64 "\n", name,
		    (uint64_t)parentClassValue);
	}

	Obj *parentClassObj = get_object_value(parentClassValue);
	if (!parentClassObj) {
		errors::wrenAbort("Cannot inherit null parent class for '%s'\n", name);
	}

	ObjClass *parentClass = dynamic_cast<ObjClass *>(parentClassObj);
	if (!parentClass) {
		errors::wrenAbort("Cannot inherit from non-class object '%s' for '%s'\n", parentClassObj->type->name.c_str(),
		    name);
	}

	// System classes poke their methods into the parent class
	if (spec->isSystemClass) {
		// Adding and using fields is off the table, as the class layout is defined in C++
		if (!spec->fields.empty()) {
			errors::wrenAbort("Supposed system class '%s' cannot add fields!\n", name);
		}

		ObjClass *cls = (ObjClass *)get_object_value(wren_get_core_class_value(name));

		if (cls == nullptr) {
			errors::wrenAbort("Supposed system class '%s' has null C++ version!\n", name);
		}

		// Set up the parent field, this is later used in WrenRuntime::Initialise to set up all the inherited functions
		if (cls->parentClass != nullptr) {
			errors::wrenAbort("System class '%s' already has a parent set!\n", name);
		}
		cls->parentClass = parentClass;

		for (const ClassDescription::MethodDecl &method : spec->methods) {
			ObjClass *target = method.isStatic ? cls->type : cls;
			// Pass false so we don't overwrite functions defined in the C++ class, as they should
			// be preferred for performance.
			target->AddFunction(method.name, method.func, false);
		}
		return NULL_VAL;
	}

	ObjManagedClass *cls = WrenRuntime::Instance().New<ObjManagedClass>(name, std::move(spec), parentClass);

	for (const ClassDescription::MethodDecl &method : cls->spec->methods) {
		ObjClass *target = method.isStatic ? cls->type : cls;
		target->AddFunction(method.name, method.func);
	}

	return encode_object(cls);
}

Value wren_alloc_obj(Value classVar) {
	if (!is_object(classVar)) {
		errors::wrenAbort("Cannot call wren_alloc_object with number argument\n");
	}

	ObjManagedClass *cls = dynamic_cast<ObjManagedClass *>(get_object_value(classVar));
	if (!cls) {
		errors::wrenAbort("Cannot call wren_alloc_object with null or non-ObjManagedClass type\n");
	}

	// We have to allocate managed objects specially, to account for their variable-sized field area
	void *mem = WrenRuntime::Instance().AllocateMem(cls->size, alignof(ObjManaged));
	memset(mem, 0, cls->size);                   // Zero as a matter of good practice
	ObjManaged *obj = new (mem) ObjManaged(cls); // Initialise with placement-new

	// Null-initialise all the fields
	Value *fieldsEnd = (Value *)((uint64_t)obj + cls->size);
	for (Value *i = obj->fields; i < fieldsEnd; i++) {
		*i = NULL_VAL;
	}

	return encode_object(obj);
}

int wren_class_get_field_offset(Value classVar) {
	if (!is_object(classVar)) {
		errors::wrenAbort("Cannot call wren_class_get_field_offset with number argument\n");
	}

	ObjManagedClass *cls = dynamic_cast<ObjManagedClass *>(get_object_value(classVar));
	if (!cls) {
		errors::wrenAbort("Cannot call wren_class_get_field_offset with null or non-ObjManagedClass type\n");
	}

	return cls->fieldOffset;
}

ClosureSpec *wren_register_closure(void *specData) {
	// Leaks memory, but it'd never be freed anyway since it gets put in a module-level global
	return new ClosureSpec(specData);
}

Value wren_create_closure(ClosureSpec *spec, void *stack, void *upvalueTable, ObjFn **listHead) {
	if (spec == nullptr) {
		errors::wrenAbort("Cannot pass null spec to wren_create_closure\n");
	}

	// Stack may be null if we have no upvalues
	ObjFn *closure = WrenRuntime::Instance().New<ObjFn>(spec, stack, upvalueTable);

	// Add this object to the linked list of all the other functions of the same type that have been created
	// This is used for tracking which closures need to be fixed up when their upvalues escape.
	// If this closure doesn't use upvalues, then listHead will be null as there's no need to track it.
	if (listHead) {
		closure->upvalueFixupList = *listHead;
		*listHead = closure;
	}

	return closure->ToValue();
}

Value **wren_get_closure_upvalue_pack(ObjFn *closure) { return closure->upvaluePointers.data(); }
ObjFn *wren_get_closure_chain_next(ObjFn *closure) {
	// TODO implement
	return nullptr;
}

// Allocate space for a closed upvalue on the heap, move the value there, and update all the closures that
// point to that value over to the new storage location.
void *wren_alloc_upvalue_storage(int numClosures) {
	// TODO reference-counting stuff
	return WrenRuntime::Instance().AllocateMem(sizeof(Value) * numClosures, 8);
}

Value wren_get_bool_value(bool value) { return encode_object(ObjBool::Get(value)); }

Value wren_get_core_class_value(const char *name) {
#define GET_CLASS(cls_name, obj)                                                                                       \
	do {                                                                                                               \
		if (strcmp(name, cls_name) == 0)                                                                               \
			return (obj);                                                                                              \
	} while (0)

	GET_CLASS("Bool", ObjBool::Class()->ToValue());
	GET_CLASS("Object", CoreClasses::Instance()->Object().ToValue());
	GET_CLASS("Class", CoreClasses::Instance()->RootClass().ToValue());
	GET_CLASS("Fn", ObjFn::Class()->ToValue());
	GET_CLASS("Fiber", ObjFibre::Class()->ToValue());
	GET_CLASS("List", ObjList::Class()->ToValue());
	GET_CLASS("Num", ObjNumClass::Instance()->ToValue());
	GET_CLASS("String", ObjString::Class()->ToValue());
	GET_CLASS("System", CoreClasses::Instance()->System()->ToValue());
	GET_CLASS("Range", ObjRange::Class()->ToValue());
	GET_CLASS("Null", ObjNull::Class()->ToValue());
	GET_CLASS("Map", ObjMap::Class()->ToValue());
	GET_CLASS("Sequence", ObjSequence::Class()->ToValue());

	errors::wrenAbort("Module requested unknown system class '%s', aborting\n", name);

#undef GET_CLASS
}

RtModule *wren_import_module(const char *name, void *getGlobalsFunc) {
	return WrenRuntime::Instance().GetOrInitModule(getGlobalsFunc);
}

Value wren_get_module_global(RtModule *mod, const char *name) {
	Value *ptr = mod->GetOrNull(name);
	if (!ptr) {
		errors::wrenAbort("No such module variable: %s.%s", mod->moduleName.c_str(), name);
	}

	return *ptr;
}
