//
// Created by znix on 11/02/23.
//

// Use our standard DLL_EXPORT macro for the Wren functions
#include "common/HashUtil.h"
#include "common/common.h"
#define WREN_API DLL_EXPORT

#include "WrenAPIPublic.h"

#include "WrenAPI.h"

#include "Errors.h"
#include "ObjBool.h"
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
#include <vector>

static std::optional<WrenConfiguration> currentConfiguration;
static ModuleNameTransformer moduleNameTransformer = nullptr;

using api_interface::ForeignClassInterface;

// WrenVM instances are only used for FFI-type stuff, so they're
// basically just a stack.
struct WrenVM {
	std::vector<Value> stack;

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

// TODO mark these as GC roots!
struct WrenHandle {
	// For value handles
	Value value = NULL_VAL;

	// For function handles
	SignatureId signature;
	std::string signatureStr;
	int arity = -1;
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

void wrencSetModuleNameTransformer(ModuleNameTransformer transformer) { moduleNameTransformer = transformer; }

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

// Get slot functions

bool wrenGetSlotBool(WrenVM *vm, int slot) {
	ObjBool *obj = vm->GetSlotAsObject<ObjBool>(slot, "Bool", "GetSlotBool");
	return obj->AsBool();
}

const char *wrenGetSlotBytes(WrenVM *vm, int slot, int *length) {
	ObjString *obj = vm->GetSlotAsObject<ObjString>(slot, "String", "GetSlotBytes");
	*length = obj->m_value.length();
	return obj->m_value.c_str();
}

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

const char *wrenGetSlotString(WrenVM *vm, int slot) {
	ObjString *obj = vm->GetSlotAsObject<ObjString>(slot, "String", "GetSlotString");
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

void wrenSetSlotNewList(WrenVM *vm, int slot) { TODO; }

int wrenGetListCount(WrenVM *vm, int slot) {
	ObjList *list = vm->GetSlotAsObject<ObjList>(slot, "List", "GetListCount");
	return list->Count();
}

void wrenGetListElement(WrenVM *vm, int listSlot, int index, int elementSlot) {
	ObjList *list = vm->GetSlotAsObject<ObjList>(listSlot, "List", "GetListElement");
	vm->stack.at(elementSlot) = list->items.at(index);
}

void wrenSetListElement(WrenVM *vm, int listSlot, int index, int elementSlot) { TODO; }
void wrenInsertInList(WrenVM *vm, int listSlot, int index, int elementSlot) { TODO; }

// Map functions

void wrenSetSlotNewMap(WrenVM *vm, int slot) { TODO; }
int wrenGetMapCount(WrenVM *vm, int slot) { TODO; }
bool wrenGetMapContainsKey(WrenVM *vm, int mapSlot, int keySlot) { TODO; }

void wrenGetMapValue(WrenVM *vm, int mapSlot, int keySlot, int valueSlot) {
	ObjMap *map = vm->GetSlotAsObject<ObjMap>(mapSlot, "Map", "GetMapElement");
	Value key = vm->stack.at(keySlot);
	Value result = map->OperatorSubscript(key);
	vm->stack.at(valueSlot) = result;
}

void wrenSetMapValue(WrenVM *vm, int mapSlot, int keySlot, int valueSlot) { TODO; }
void wrenRemoveMapValue(WrenVM *vm, int mapSlot, int keySlot, int removedValueSlot) { TODO; }

// Handle functions

WrenHandle *wrenGetSlotHandle(WrenVM *vm, int slot) {
	Value value = vm->stack.at(slot);
	return new WrenHandle{.value = value};
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

	return new WrenHandle{.signature = sigId, .signatureStr = signature, .arity = arity};
}
WrenInterpretResult wrenCall(WrenVM *vm, WrenHandle *method) {
	if ((int)vm->stack.size() < method->arity) {
		errors::wrenAbort("MakeCallHandle: Insufficient stack slots for arity %d", method->arity - 1);
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
		errors::wrenAbort("MakeCallHandle: %s does not implement '%s'.", type->name.c_str(),
		    method->signatureStr.c_str());
	}

	Value result = ObjFn::FunctionDispatch(func->func, nullptr, method->arity, vm->stack.data());
	vm->stack.at(0) = result;

	// Required for the call.wren test
	vm->stack.resize(1);

	// TODO error handling
	return WREN_RESULT_SUCCESS;
}

void wrenReleaseHandle(WrenVM *vm, WrenHandle *handle) { delete handle; }

// Execution, runtime and fibre functions

int wrenGetVersionNumber() { return WREN_VERSION_NUMBER; }

void wrenAbortFiber(WrenVM *vm, int slot) { TODO; }
void *wrenGetUserData(WrenVM *vm) { TODO; }
void wrenSetUserData(WrenVM *vm, void *userData) { TODO; }

void wrenGetVariable(WrenVM *vm, const char *modName, const char *name, int slot) {
	RtModule *mod = lookupModule(modName);
	Value *value = mod->GetOrNull(name);
	vm->stack.at(slot) = *value;
}
bool wrenHasVariable(WrenVM *vm, const char *modName, const char *name) { TODO; }
bool wrenHasModule(WrenVM *vm, const char *modName) { TODO; }

WrenVM *wrenNewVM(WrenConfiguration *configuration) {
	// Due to our use of global variables in generated code, there really
	// can only be one VM.
	// We'll return a sort-of dummy VM for the user to call functions etc.
	currentConfiguration = *configuration;
	return new WrenVM;
}

void wrenFreeVM(WrenVM *vm) { delete vm; }

void wrenInitConfiguration(WrenConfiguration *configuration) { *configuration = {}; }

WrenInterpretResult wrenInterpret(WrenVM *vm, const char *modName, const char *source) { TODO; }
