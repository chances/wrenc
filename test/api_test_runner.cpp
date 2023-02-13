//
// Created by znix on 12/02/23.
//

#include "pub_include/wren.h"

extern "C" {
#define wren_h // Avoid importing the Wren API twice
#include "api_tests.h"
}

#include <dlfcn.h>
#include <stdio.h>
#include <string>
#include <vector>

static const char *testModule = nullptr;

static char *nameTransformer(const char *originalName) {
	std::string name = originalName;

	// Additional modules that are imported into some tests
	static std::vector<std::string> utilModules = {
	    "get_variable_module", // From get_variable
	    "not a module",        // This isn't supposed to exist, so renaming it won't help
	};

	for (const std::string &utilName : utilModules) {
		if (name.ends_with(utilName))
			return strdup(utilName.c_str());
	}

	// The real module is always named 'test', but the testing
	// C code doesn't know that.
	return strdup("test");
}

static WrenForeignMethodFn bindMethodWrapper(WrenVM *vm, const char *mod, const char *className, bool isStatic,
    const char *signature) {

	// Edit the module name, to account for the differences in the test environment
	if (strcmp(mod, "test") == 0) {
		mod = "./test/api/whatever";
	}

	return APITest_bindForeignMethod(vm, mod, className, isStatic, signature);
}

static WrenForeignClassMethods bindClassWrapper(WrenVM *vm, const char *mod, const char *className) {
	// Edit the module name, to account for the differences in the test environment
	if (strcmp(mod, "test") == 0) {
		mod = "./test/api/whatever";
	}

	return APITest_bindForeignClass(vm, mod, className);
}

static void reportError(WrenVM *vm, WrenErrorType type, const char *module, int line, const char *message) {
	// Copied from Wren's test.c
	switch (type) {
	case WREN_ERROR_COMPILE:
		fprintf(stderr, "[%s line %d] %s\n", module, line, message);
		break;

	case WREN_ERROR_RUNTIME:
		fprintf(stderr, "%s\n", message);
		break;

	case WREN_ERROR_STACK_TRACE:
		fprintf(stderr, "[%s line %d] in %s\n", module, line, message);
		break;
	}
}

static void writeImpl(const char *message, int length) { printf("%s", message); }

int main(int argc, char **argv) {
	// Set the write handler here rather than through WrenConfiguration, so
	// it doesn't get changed if someone calls wrenNewVM.
	wrencSetNullSafeWriteFn(writeImpl);

	if (argc != 3) {
		fprintf(stderr, "Usage: %s <test shared library> <test path>\n", argv[1]);
		return 1;
	}

	testModule = argv[2];

	void *handle = dlopen(argv[1], RTLD_NOW);
	if (!handle) {
		fprintf(stderr, "Failed to load test library: %s\n", dlerror());
		return 1;
	}

	// The module under test is always named 'test', hence the symbol name.
	void *getGlobalsFunc = dlsym(handle, "test_get_globals");
	if (!getGlobalsFunc) {
		fprintf(stderr, "Failed to resolve the get-globals symbol: %s\n", dlerror());
		return 1;
	}

	// Create the VM object
	WrenConfiguration config;
	wrenInitConfiguration(&config);
	config.bindForeignMethodFn = bindMethodWrapper;
	config.bindForeignClassFn = bindClassWrapper;
	config.errorFn = reportError;
	WrenVM *vm = wrenNewVM(&config);

	// Transform all the module names in the API, to avoid
	// modifying the API tests.
	wrencSetModuleNameTransformer(nameTransformer);

	// Initialise and run the main module
	bool success = wrencInitModule(getGlobalsFunc);
	if (!success) {
		return 1;
	}

	// Run the API tests
	int exitCode = APITest_Run(vm, testModule);

	wrenFreeVM(vm);

	if (dlclose(handle)) {
		fprintf(stderr, "Failed to close test library: %s\n", dlerror());
		return 1;
	}

	return exitCode;
}
