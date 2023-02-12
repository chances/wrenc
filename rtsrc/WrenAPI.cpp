//
// Created by znix on 11/02/23.
//

// Use our standard DLL_EXPORT macro for the Wren functions
#include "common/common.h"
#define WREN_API DLL_EXPORT

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

#define TODO abort()

// Get slot functions

bool wrenGetSlotBool(WrenVM *vm, int slot) { TODO; }
const char *wrenGetSlotBytes(WrenVM *vm, int slot, int *length) { TODO; }

double wrenGetSlotDouble(WrenVM *vm, int slot) {
	Value value = vm->stack.at(slot);
	if (!is_value_float(value))
		errors::wrenAbort("Cannot call GetSlotDouble on slot %d that contains an object", slot);

	return get_number_value(value);
}

void *wrenGetSlotForeign(WrenVM *vm, int slot) {
	ObjManaged *obj = vm->GetSlotAsObject<ObjManaged>(slot, "ObjManaged", "GetSlotForeign");
	ObjManagedClass *cls = (ObjManagedClass *)obj->type;
	if (!cls->foreignClass) {
		errors::wrenAbort("Cannot get foreign class data from non-foreign class '%s'", cls->name.c_str());
	}
	return obj->fields;
}

const char *wrenGetSlotString(WrenVM *vm, int slot) { TODO; }

// Set slot functions

void wrenSetSlotBool(WrenVM *vm, int slot, bool value) { TODO; }

void wrenSetSlotBytes(WrenVM *vm, int slot, const char *bytes, size_t length) { TODO; }

void wrenSetSlotDouble(WrenVM *vm, int slot, double value) { vm->stack.at(slot) = encode_number(value); }

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

void wrenSetSlotNull(WrenVM *vm, int slot) { TODO; }
void wrenSetSlotString(WrenVM *vm, int slot, const char *text) { TODO; }

// Misc slot functions

int wrenGetSlotCount(WrenVM *vm) { TODO; }
void wrenEnsureSlots(WrenVM *vm, int numSlots) { TODO; }
WrenType wrenGetSlotType(WrenVM *vm, int slot) { TODO; }

// List functions

void wrenSetSlotNewList(WrenVM *vm, int slot) { TODO; }
int wrenGetListCount(WrenVM *vm, int slot) { TODO; }
void wrenGetListElement(WrenVM *vm, int listSlot, int index, int elementSlot) { TODO; }
void wrenSetListElement(WrenVM *vm, int listSlot, int index, int elementSlot) { TODO; }
void wrenInsertInList(WrenVM *vm, int listSlot, int index, int elementSlot) { TODO; }

// Map functions

void wrenSetSlotNewMap(WrenVM *vm, int slot) { TODO; }
int wrenGetMapCount(WrenVM *vm, int slot) { TODO; }
bool wrenGetMapContainsKey(WrenVM *vm, int mapSlot, int keySlot) { TODO; }
void wrenGetMapValue(WrenVM *vm, int mapSlot, int keySlot, int valueSlot) { TODO; }
void wrenSetMapValue(WrenVM *vm, int mapSlot, int keySlot, int valueSlot) { TODO; }
void wrenRemoveMapValue(WrenVM *vm, int mapSlot, int keySlot, int removedValueSlot) { TODO; }

// Handle functions

WrenHandle *wrenGetSlotHandle(WrenVM *vm, int slot) { TODO; }
void wrenSetSlotHandle(WrenVM *vm, int slot, WrenHandle *handle) { TODO; }

WrenHandle *wrenMakeCallHandle(WrenVM *vm, const char *signature) { TODO; }
WrenInterpretResult wrenCall(WrenVM *vm, WrenHandle *method) { TODO; }

void wrenReleaseHandle(WrenVM *vm, WrenHandle *handle) { TODO; }

// Execution, runtime and fibre functions

int wrenGetVersionNumber() { return WREN_VERSION_NUMBER; }

void wrenAbortFiber(WrenVM *vm, int slot) { TODO; }
void *wrenGetUserData(WrenVM *vm) { TODO; }
void wrenSetUserData(WrenVM *vm, void *userData) { TODO; }

void wrenGetVariable(WrenVM *vm, const char *module, const char *name, int slot) { TODO; }
bool wrenHasVariable(WrenVM *vm, const char *module, const char *name) { TODO; }
bool wrenHasModule(WrenVM *vm, const char *module) { TODO; }

WrenVM *wrenNewVM(WrenConfiguration *configuration) {
	// Due to our use of global variables in generated code, there really
	// can only be one VM.
	// TODO how we'll match up this API difference.
	currentConfiguration = *configuration;
	return nullptr;
}

void wrenFreeVM(WrenVM *vm) { TODO; }

void wrenInitConfiguration(WrenConfiguration *configuration) { TODO; }

WrenInterpretResult wrenInterpret(WrenVM *vm, const char *module, const char *source) { TODO; }
