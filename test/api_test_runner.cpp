//
// Created by znix on 12/02/23.
//

#include "../rtsrc/WrenAPIPublic.h"
#include "../rtsrc/WrenRuntime.h"

extern "C" {
#define wren_h // Avoid importing the Wren API twice
#include "api_tests.h"
}

#include <dlfcn.h>
#include <stdio.h>

typedef Value (*wren_main_func_t)();

static const char *testModule = nullptr;

static char *nameTransformer(const char *originalName) {
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

int main(int argc, char **argv) {
	// Disable stdout buffering. Our System.write uses the platform write
	// call directly, rather than printf. Thus the messages may end up
	// out of order between wren and native output if buffering is enabled.
	setvbuf(stdout, nullptr, _IONBF, 0);
	setvbuf(stderr, nullptr, _IONBF, 0);

	WrenRuntime::Initialise();

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
	WrenConfiguration config = {
	    .bindForeignMethodFn = bindMethodWrapper,
	    .bindForeignClassFn = bindClassWrapper,
	};
	WrenVM *vm = wrenNewVM(&config);

	// Transform all the module names in the API, to avoid
	// modifying the API tests.
	wrencSetModuleNameTransformer(nameTransformer);

	// Initialise and run the main module
	WrenRuntime::Instance().GetOrInitModuleCaught(getGlobalsFunc);

	// Run the API tests
	int exitCode = APITest_Run(vm, testModule);

	wrenFreeVM(vm);

	if (dlclose(handle)) {
		fprintf(stderr, "Failed to close test library: %s\n", dlerror());
		return 1;
	}

	return exitCode;
}
