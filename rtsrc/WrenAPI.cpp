//
// Created by znix on 11/02/23.
//

#include "WrenAPIPublic.h"

#include "WrenAPI.h"

#include "Errors.h"
#include "RtModule.h"

#include <deque>
#include <optional>

static std::optional<WrenConfiguration> currentConfiguration;

// WrenVM instances are only used for FFI-type stuff, so they're
// basically just a stack.
struct WrenVM {
	std::deque<Value> stack;
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
	if (!currentConfiguration) {
		errors::wrenAbort("Could not look up foreign method '%s' without configuration set.", method.name.c_str());
	}

	WrenBindForeignMethodFn bindFunc = currentConfiguration->bindForeignMethodFn;
	if (!currentConfiguration) {
		errors::wrenAbort("Could not look up foreign method '%s' without binding function.", method.name.c_str());
	}

	WrenForeignMethodFn func =
	    bindFunc(nullptr, mod->moduleName.c_str(), className.c_str(), method.isStatic, method.name.c_str());
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
