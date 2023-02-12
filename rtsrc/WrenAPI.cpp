//
// Created by znix on 11/02/23.
//

#include "WrenAPIPublic.h"

#include "WrenAPI.h"

#include "Errors.h"
#include "ObjManaged.h"
#include "RtModule.h"
#include "SlabObjectAllocator.h"
#include "WrenRuntime.h"

#include "random/random_native.h"

#include <deque>
#include <optional>

static std::optional<WrenConfiguration> currentConfiguration;

using api_interface::ForeignClassInterface;

// WrenVM instances are only used for FFI-type stuff, so they're
// basically just a stack.
struct WrenVM {
	std::deque<Value> stack;

	template <typename T> T *GetSlotAsObject(int slot, const char *typeName, const char *msg) {
		if (slot < 0)
			errors::wrenAbort("%s: invalid negative slot index %d", msg, slot);
		if (slot >= (int)stack.size())
			errors::wrenAbort("%s: too large slot index %d (max %d)", msg, slot, (int)stack.size());

		Value value = stack.at(slot);
		if (!is_object(value))
			errors::wrenAbort("%s: expected object at slot %d, found number", msg, slot);

		Obj *baseObj = get_object_value(value);
		if (baseObj == nullptr)
			return nullptr;

		T *obj = dynamic_cast<T *>(baseObj);
		if (obj == nullptr)
			errors::wrenAbort("%s: expected object of type '%s', got '%s'", msg, typeName, baseObj->type->name.c_str());

		return obj;
	}
};

int wrenGetVersionNumber() { return WREN_VERSION_NUMBER; }

WrenVM *wrenNewVM(WrenConfiguration *configuration) {
	// Due to our use of global variables in generated code, there really
	// can only be one VM.
	// TODO how we'll match up this API difference.
	currentConfiguration = *configuration;
	return nullptr;
}

void *api_interface::lookupForeignMethod(RtModule *mod, const std::string &className,
    const ClassDescription::MethodDecl &method) {

	WrenForeignMethodFn func;

	// Try looking the function up in the loosely-built-in random module
	func = wren_random::bindRandomForeignMethod(mod->moduleName, className, method.isStatic, method.name);
	if (func != nullptr) {
		return (void *)func;
	}

	if (!currentConfiguration) {
		errors::wrenAbort("Could not look up foreign method '%s' without configuration set.", method.name.c_str());
	}

	WrenBindForeignMethodFn bindFunc = currentConfiguration->bindForeignMethodFn;
	if (!currentConfiguration) {
		errors::wrenAbort("Could not look up foreign method '%s' without binding function.", method.name.c_str());
	}

	func = bindFunc(nullptr, mod->moduleName.c_str(), className.c_str(), method.isStatic, method.name.c_str());
	if (!func) {
		errors::wrenAbort("Could not find foreign method '%s' for class %s in module '%s'.", method.name.c_str(),
		    className.c_str(), mod->moduleName.c_str());
	}

	return (void *)func;
}

Value api_interface::dispatchForeignCall(void *funcPtr, Value *args, int argsLen) {
	WrenForeignMethodFn fn = (WrenForeignMethodFn)funcPtr;

	WrenVM vm;
	vm.stack.insert(vm.stack.begin(), args, args + argsLen);

	fn(&vm);

	// Variables are returned in slot 0
	return vm.stack.at(0);
}

std::unique_ptr<ForeignClassInterface> ForeignClassInterface::Lookup(RtModule *mod, const std::string &className) {
	WrenForeignClassMethods fcm;

	auto convert = [&]() {
		auto fci = std::make_unique<ForeignClassInterface>();
		fci->m_allocate = (void *)fcm.allocate;
		fci->m_deallocate = (void *)fcm.finalize;
		return fci;
	};

	// Try looking the function up in the loosely-built-in random module
	fcm = wren_random::bindRandomForeignClass(mod->moduleName, className);
	if (fcm.allocate) {
		return convert();
	}

	// TODO configuration-based foreign class lookup
	abort();
}

ObjManaged *ForeignClassInterface::Allocate(ObjManagedClass *cls) {
	WrenVM vm;
	vm.stack.push_back(encode_object(cls));

	WrenForeignMethodFn allocFn = (WrenForeignMethodFn)m_allocate;
	allocFn(&vm);

	if (!is_object(vm.stack.at(0))) {
		errors::wrenAbort("Error initialising foreign class '%s': returned a number", cls->name.c_str());
	}
	ObjManaged *result = dynamic_cast<ObjManaged *>(get_object_value(vm.stack.at(0)));
	if (!result) {
		errors::wrenAbort("Error initialising foreign class '%s': returned a non-managed class", cls->name.c_str());
	}
	if (result->type != cls) {
		errors::wrenAbort("Error initialising foreign class '%s': returned the wrong class '%s'", cls->name.c_str(),
		    result->type->name.c_str());
	}
	return result;
}

void ForeignClassInterface::Finalise(ObjManaged *obj) {}

// -------------------------------------
// --- Wren API function definitions ---
// -------------------------------------

// Get slot functions

void *wrenSetSlotNewForeign(WrenVM *vm, int slot, int classSlot, size_t size) {
	ObjManagedClass *cls = vm->GetSlotAsObject<ObjManagedClass>(classSlot, "ObjManagedClass", "SetSlotNewForeign");

	// Create the object of the specified size
	static_assert(alignof(ObjManaged) == 8);
	void *mem = WrenRuntime::Instance().GetObjectAllocator()->AllocateRaw(sizeof(ObjManaged) + size);
	ObjManaged *obj = new (mem) ObjManaged(cls); // Initialise with placement-new

	vm->stack.at(slot) = encode_object(obj);

	// The 'fields' array happens to be the variable-sized thing
	// at the end, so it gets to represent the native data.
	return (void *)obj->fields;
}

void *wrenGetSlotForeign(WrenVM *vm, int slot) {
	ObjManaged *obj = vm->GetSlotAsObject<ObjManaged>(slot, "ObjManaged", "GetSlotForeign");
	ObjManagedClass *cls = (ObjManagedClass *)obj->type;
	if (!cls->foreignClass) {
		errors::wrenAbort("Cannot get foreign class data from non-foreign class '%s'", cls->name.c_str());
	}
	return obj->fields;
}

double wrenGetSlotDouble(WrenVM *vm, int slot) {
	Value value = vm->stack.at(slot);
	if (!is_value_float(value))
		errors::wrenAbort("Cannot call GetSlotDouble on slot %d that contains an object", slot);

	return get_number_value(value);
}

// Set slot functions

void wrenSetSlotDouble(WrenVM *vm, int slot, double value) { vm->stack.at(slot) = encode_number(value); }
