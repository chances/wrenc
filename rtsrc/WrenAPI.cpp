//
// Created by znix on 11/02/23.
//

// Use our standard DLL_EXPORT macro for the Wren functions
#include "common/HashUtil.h"
#include "common/common.h"
#define WREN_API DLL_EXPORT

#include "pub_include/wren.h"

#include "WrenAPI.h"

#include "Errors.h"
#include "ObjBool.h"
#include "ObjFibre.h"
#include "ObjFn.h"
#include "ObjList.h"
#include "ObjManaged.h"
#include "ObjMap.h"
#include "ObjString.h"
#include "RtModule.h"
#include "SlabObjectAllocator.h"
#include "WrenRuntime.h"

#include "random/random_native.h"

#include <optional>
#include <set>
#include <vector>

static std::optional<WrenConfiguration> currentConfiguration;
static WrencModuleNameTransformer moduleNameTransformer = nullptr;

// Keep a list of all the active handles, so they can be marked
// as GC roots.
static std::set<WrenHandle *> currentHandles;

// Have a list of all the VMs around so we can mark their stack
// as GC roots.
static std::set<WrenVM *> currentVMs;

// Since it's not really possible to have multiple independent
// VMs, trying to store this inside the VM and keep track of
// it across function calls and fibres would be a huge pain.
static void *vmUserData = nullptr;

using api_interface::ForeignClassInterface;

static void writeError(const char *format, ...) MARK_PRINTF_FORMAT(1, 2);

// WrenVM instances are only used for FFI-type stuff, so they're
// basically just a stack.
struct WrenVM {
	std::vector<Value> stack;

	WrenVM() { currentVMs.insert(this); }
	~WrenVM() { currentVMs.erase(this); }

	template <typename T> T *GetSlotAsObject(int slot, const char *typeName, const char *msg) {
		if (slot < 0) {
			writeError("%s: invalid negative slot index %d", msg, slot);
			return nullptr;
		}
		if (slot >= (int)stack.size()) {
			writeError("%s: too large slot index %d (max %d)", msg, slot, (int)stack.size());
			return nullptr;
		}

		Value value = stack.at(slot);
		if (!is_object(value)) {
			writeError("%s: expected object at slot %d, found number", msg, slot);
			return nullptr;
		}

		Obj *baseObj = get_object_value(value);
		if (baseObj == nullptr) {
			writeError("%s: expected object at slot %d, found null", msg, slot);
			return nullptr;
		}

		T *obj = dynamic_cast<T *>(baseObj);
		if (obj == nullptr) {
			writeError("%s: expected object of type '%s', got '%s'", msg, typeName, baseObj->type->name.c_str());
			return nullptr;
		}

		return obj;
	}
};

struct WrenHandle {
	// For value handles
	Value value = NULL_VAL;

	// For function handles
	SignatureId signature;
	std::string signatureStr;
	int arity = -1;

	WrenHandle() { currentHandles.insert(this); }
	~WrenHandle() { currentHandles.erase(this); }
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
	if (!bindFunc) {
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

void api_interface::markGCRoots(GCMarkOps *ops) {
	for (WrenVM *vm : currentVMs) {
		ops->ReportValues(ops, vm->stack.data(), vm->stack.size());
	}
	for (WrenHandle *handle : currentHandles) {
		ops->ReportValue(ops, handle->value);
	}
}

void api_interface::systemPrintImpl(const std::string &message) {
	if (!currentConfiguration)
		return;
	WrenWriteFn write = currentConfiguration->writeFn;
	if (write) {
		write(nullptr, message.c_str());
	}
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

	if (!currentConfiguration) {
		errors::wrenAbort("Could not look up foreign class '%s' without configuration set.", className.c_str());
	}

	WrenBindForeignClassFn bindFunc = currentConfiguration->bindForeignClassFn;
	if (!bindFunc) {
		errors::wrenAbort("Could not look up foreign class '%s' without binding function.", className.c_str());
	}

	fcm = bindFunc(nullptr, mod->moduleName.c_str(), className.c_str());
	if (!fcm.allocate) {
		errors::wrenAbort("Could not find foreign class '%s' in module '%s'.", className.c_str(),
		    mod->moduleName.c_str());
	}

	return convert();
}

ObjManaged *ForeignClassInterface::Allocate(ObjManagedClass *cls, Value *args, int arity) {
	WrenVM vm;
	vm.stack.push_back(encode_object(cls));

	// Put all the constructor arguments on the stack, as the allocator may use them.
	vm.stack.insert(vm.stack.begin() + 1, args, args + arity);

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

void ForeignClassInterface::Finalise(ObjManaged *obj) {
	// Note the finaliser is optional, classes that just store data and
	// don't need any cleanup beyond that being freed don't need them.
	WrenFinalizerFn fn = (WrenFinalizerFn)m_deallocate;
	if (fn) {
		fn(obj->fields);
	}
}

void writeError(const char *format, ...) {
	char buffer[256];
	va_list args;
	va_start(args, format);
	vsnprintf(buffer, sizeof(buffer), format, args);
	va_end(args);

	WrenErrorFn errorFn = nullptr;
	if (currentConfiguration) {
		errorFn = currentConfiguration->errorFn;
	}

	if (errorFn) {
		errorFn(nullptr, WREN_ERROR_RUNTIME, "<api>", -1, buffer);
	} else {
		// Even if the application doesn't set an error handler, print
		// it anyway since this could otherwise be a very nasty foot-gun.
		fprintf(stderr, "Error in Wren API: %s\n", buffer);
	}
}

void wrencSetModuleNameTransformer(WrencModuleNameTransformer transformer) { moduleNameTransformer = transformer; }

static std::string transformModuleName(const char *name) {
	if (!moduleNameTransformer)
		return name;

	char *modified = moduleNameTransformer(name);
	if (!modified)
		return name;

	std::string str = modified;
	free(modified);
	return str;
}

static RtModule *lookupModule(const char *name) {
	std::string newName = transformModuleName(name);
	return WrenRuntime::Instance().GetModuleByName(newName);
}

// -------------------------------------
// --- Wren API function definitions ---
// -------------------------------------

#define TODO                                                                                                           \
	do {                                                                                                               \
		fprintf(stderr, "TODO implement API method: %s\n", __func__);                                                  \
		abort();                                                                                                       \
	} while (1)

#define GET_OBJ(var, type, errorValue, slot)                                                                           \
	type *var;                                                                                                         \
	do {                                                                                                               \
		var = vm->GetSlotAsObject<type>(slot, #type, __func__);                                                        \
		if (!var) {                                                                                                    \
			/* An error has already been printed by GetSlotAsObject */                                                 \
			return errorValue;                                                                                         \
		}                                                                                                              \
	} while (0)

// Get slot functions

bool wrenGetSlotBool(WrenVM *vm, int slot) {
	GET_OBJ(obj, ObjBool, false, slot);
	return obj->AsBool();
}

const char *wrenGetSlotBytes(WrenVM *vm, int slot, int *length) {
	*length = 0; // Zero the length in case of error
	GET_OBJ(obj, ObjString, nullptr, slot);
	*length = obj->m_value.length();
	return obj->m_value.c_str();
}

double wrenGetSlotDouble(WrenVM *vm, int slot) {
	Value value = vm->stack.at(slot);
	if (!is_value_float(value)) {
		writeError("Cannot call GetSlotDouble on slot %d that contains an object", slot);
		return 0;
	}

	return get_number_value(value);
}

void *wrenGetSlotForeign(WrenVM *vm, int slot) {
	GET_OBJ(obj, ObjManaged, nullptr, slot);
	ObjManagedClass *cls = (ObjManagedClass *)obj->type;
	if (!cls->foreignClass) {
		writeError("Cannot get foreign class data from non-foreign class '%s'", cls->name.c_str());
		return nullptr;
	}
	return obj->fields;
}

const char *wrenGetSlotString(WrenVM *vm, int slot) {
	GET_OBJ(obj, ObjString, nullptr, slot);
	return obj->m_value.c_str();
}

// Set slot functions

void wrenSetSlotBool(WrenVM *vm, int slot, bool value) { vm->stack.at(slot) = encode_object(ObjBool::Get(value)); }

void wrenSetSlotBytes(WrenVM *vm, int slot, const char *bytes, size_t length) {
	std::string str(bytes, length);
	vm->stack.at(slot) = encode_object(ObjString::New(std::move(str)));
}

void wrenSetSlotDouble(WrenVM *vm, int slot, double value) { vm->stack.at(slot) = encode_number(value); }

void *wrenSetSlotNewForeign(WrenVM *vm, int slot, int classSlot, size_t size) {
	GET_OBJ(cls, ObjManagedClass, nullptr, classSlot);

	// Create the object of the specified size
	static_assert(alignof(ObjManaged) == 8);
	void *mem = WrenRuntime::Instance().GetObjectAllocator()->AllocateRaw(sizeof(ObjManaged) + size);
	ObjManaged *obj = new (mem) ObjManaged(cls); // Initialise with placement-new

	vm->stack.at(slot) = encode_object(obj);

	// The 'fields' array happens to be the variable-sized thing
	// at the end, so it gets to represent the native data.
	return (void *)obj->fields;
}

void wrenSetSlotNull(WrenVM *vm, int slot) { vm->stack.at(slot) = encode_object(nullptr); }

void wrenSetSlotString(WrenVM *vm, int slot, const char *text) {
	vm->stack.at(slot) = encode_object(ObjString::New(text));
}

// Misc slot functions

int wrenGetSlotCount(WrenVM *vm) { return vm->stack.size(); }

void wrenEnsureSlots(WrenVM *vm, int numSlots) {
	if ((int)vm->stack.size() < numSlots)
		vm->stack.resize(numSlots);
}

WrenType wrenGetSlotType(WrenVM *vm, int slot) {
	Value value = vm->stack.at(slot);

	if (is_value_float(value))
		return WREN_TYPE_NUM;

	Obj *obj = get_object_value(value);
	if (obj == nullptr)
		return WREN_TYPE_NULL;

	if (obj->type == ObjBool::Class())
		return WREN_TYPE_BOOL;

	if (obj->type == ObjList::Class())
		return WREN_TYPE_LIST;

	if (obj->type == ObjMap::Class())
		return WREN_TYPE_MAP;

	if (obj->type == ObjString::Class())
		return WREN_TYPE_STRING;

	// If it's not a managed class (eg, a range) then bail now
	ObjManagedClass *type = dynamic_cast<ObjManagedClass *>(obj->type);
	if (!type)
		return WREN_TYPE_UNKNOWN;

	// A foreign managed class
	if (type->foreignClass)
		return WREN_TYPE_FOREIGN;

	// Some non-foreign managed class
	return WREN_TYPE_UNKNOWN;
}

// List functions

void wrenSetSlotNewList(WrenVM *vm, int slot) { vm->stack.at(slot) = encode_object(ObjList::New()); }

int wrenGetListCount(WrenVM *vm, int slot) {
	GET_OBJ(list, ObjList, -1, slot);
	return list->Count();
}

void wrenGetListElement(WrenVM *vm, int listSlot, int index, int elementSlot) {
	GET_OBJ(list, ObjList, , listSlot);
	// Use OperatorSubscript for the negative index handling
	vm->stack.at(elementSlot) = list->OperatorSubscript(encode_number(index));
}

void wrenSetListElement(WrenVM *vm, int listSlot, int index, int elementSlot) {
	GET_OBJ(list, ObjList, , listSlot);
	list->OperatorSubscriptSet(index, vm->stack.at(elementSlot));
}

void wrenInsertInList(WrenVM *vm, int listSlot, int index, int elementSlot) {
	GET_OBJ(list, ObjList, , listSlot);
	list->Insert(index, vm->stack.at(elementSlot));
}

// Map functions

void wrenSetSlotNewMap(WrenVM *vm, int slot) { vm->stack.at(slot) = encode_object(ObjMap::New()); }

int wrenGetMapCount(WrenVM *vm, int slot) {
	GET_OBJ(map, ObjMap, -1, slot);
	return map->Count();
}

bool wrenGetMapContainsKey(WrenVM *vm, int mapSlot, int keySlot) {
	GET_OBJ(map, ObjMap, false, mapSlot);
	Value key = vm->stack.at(keySlot);
	return map->ContainsKey(key);
}

void wrenGetMapValue(WrenVM *vm, int mapSlot, int keySlot, int valueSlot) {
	GET_OBJ(map, ObjMap, , mapSlot);
	Value key = vm->stack.at(keySlot);
	Value result = map->OperatorSubscript(key);
	vm->stack.at(valueSlot) = result;
}

void wrenSetMapValue(WrenVM *vm, int mapSlot, int keySlot, int valueSlot) {
	GET_OBJ(map, ObjMap, , mapSlot);
	Value key = vm->stack.at(keySlot);
	Value value = vm->stack.at(valueSlot);
	map->OperatorSubscriptSet(key, value);
}

void wrenRemoveMapValue(WrenVM *vm, int mapSlot, int keySlot, int removedValueSlot) {
	GET_OBJ(map, ObjMap, , mapSlot);
	Value key = vm->stack.at(keySlot);
	Value oldValue = map->Remove(key);
	vm->stack.at(removedValueSlot) = oldValue;
}

// Handle functions

WrenHandle *wrenGetSlotHandle(WrenVM *vm, int slot) {
	Value value = vm->stack.at(slot);
	WrenHandle *handle = new WrenHandle;
	handle->value = value;
	return handle;
}
void wrenSetSlotHandle(WrenVM *vm, int slot, WrenHandle *handle) { vm->stack.at(slot) = handle->value; }

WrenHandle *wrenMakeCallHandle(WrenVM *vm, const char *signatureCStr) {
	std::string signature = signatureCStr;
	SignatureId sigId = hash_util::findSignatureId(signature);

	// We need to find the function's arity, so build a *very* quick
	// and hacky signature parser.
	int arity = -1;
	for (char c : signature) {
		// Wait until we find something that looks like it starts the arguments list
		if (arity == -1) {
			if (c == '(' || c == '[')
				arity = 0;
			continue;
		}
		if (arity != -1 && c == '_')
			arity++;
	}
	if (arity == -1)
		arity = 0; // Fix up getters

	// Add the receiver
	arity++;

	WrenHandle *handle = new WrenHandle;
	handle->signature = sigId;
	handle->signatureStr = signature;
	handle->arity = arity;
	return handle;
}
WrenInterpretResult wrenCall(WrenVM *vm, WrenHandle *method) {
	if ((int)vm->stack.size() < method->arity) {
		writeError("MakeCallHandle: Insufficient stack slots for arity %d", method->arity - 1);
		return WREN_RESULT_RUNTIME_ERROR;
	}

	// Sadly the receiver type can change between MakeCallHandle and Call, so we have
	// to do function lookup each time.
	// TODO remember the last type and function to skip lookups if the receiver type
	//  hasn't changed.
	Value receiver = vm->stack.at(0);

	// TODO non-object receivers (null and numbers)
	Obj *object = (Obj *)get_object_value(receiver);
	ObjClass *type = object->type;

	FunctionTable::Entry *func = type->LookupMethod(method->signature);

	if (!func) {
		writeError("MakeCallHandle: %s does not implement '%s'.", type->name.c_str(), method->signatureStr.c_str());
		return WREN_RESULT_RUNTIME_ERROR;
	}

	WrenInterpretResult error;
	try {
		Value result = ObjFn::FunctionDispatch(func->func, nullptr, method->arity, vm->stack.data());
		vm->stack.at(0) = result;
		error = WREN_RESULT_SUCCESS;
	} catch (const ObjFibre::FibreAbortException &ex) {
		vm->stack.at(0) = NULL_VAL;
		error = WREN_RESULT_RUNTIME_ERROR;

		// Try to call the error function, but if it's not set then we
		// silently swallow the error.
		std::string message = Obj::ToString(ex.message);
		WrenErrorFn errorFn = currentConfiguration->errorFn;
		if (errorFn) {
			const char *moduleName = "<unknown>";
			if (ex.originatingModule) {
				moduleName = ex.originatingModule->moduleName.c_str();
			}
			errorFn(vm, WREN_ERROR_RUNTIME, moduleName, -1, message.c_str());
		}
	}

	// Required for the call.wren test
	vm->stack.resize(1);

	return error;
}

void wrenReleaseHandle(WrenVM *vm, WrenHandle *handle) { delete handle; }

// Execution, runtime and fibre functions

int wrenGetVersionNumber() { return WREN_VERSION_NUMBER; }

void wrenAbortFiber(WrenVM *vm, int slot) { ObjFibre::Abort(vm->stack.at(slot)); }

void *wrenGetUserData(WrenVM *vm) { return vmUserData; }
void wrenSetUserData(WrenVM *vm, void *userData) { vmUserData = userData; }

void wrenGetVariable(WrenVM *vm, const char *modName, const char *name, int slot) {
	RtModule *mod = lookupModule(modName);
	Value *value = mod->GetOrNull(name);
	vm->stack.at(slot) = *value;
}

bool wrenHasVariable(WrenVM *vm, const char *modName, const char *name) {
	RtModule *mod = lookupModule(modName);
	return mod->GetOrNull(name) != nullptr;
}

bool wrenHasModule(WrenVM *vm, const char *modName) {
	RtModule *mod = lookupModule(modName);
	return mod != nullptr;
}

WrenVM *wrenNewVM(WrenConfiguration *configuration) {
	WrenRuntime::Initialise();

	// Due to our use of global variables in generated code, there really
	// can only be one VM.
	// We'll return a sort-of dummy VM for the user to call functions etc.
	currentConfiguration = *configuration;
	vmUserData = configuration->userData;
	return new WrenVM;
}

void wrenFreeVM(WrenVM *vm) { delete vm; }

void wrenInitConfiguration(WrenConfiguration *configuration) { *configuration = {}; }

WrenInterpretResult wrenInterpret(WrenVM *vm, const char *modName, const char *source) { TODO; }

void wrencSetNullSafeWriteFn(WrencWriteFnNullSafe writeFn) { WrenRuntime::Instance().SetWriteHandler(writeFn); }

bool wrencInitModule(void *moduleGetGlobals) {
	WrenRuntime::Initialise();

	try {
		WrenRuntime::Instance().GetOrInitModule(moduleGetGlobals);
		return true;
	} catch (const ObjFibre::FibreAbortException &ex) {
		std::string error = Obj::ToString(ex.message);

		if (currentConfiguration) {
			WrenErrorFn errorFn = currentConfiguration->errorFn;
			if (errorFn) {
				errorFn(nullptr, WREN_ERROR_RUNTIME, "<in module init>", -1, error.c_str());
			}
		}

		return false;
	}
}
