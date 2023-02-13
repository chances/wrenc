#include <wren.h>
#include <stdio.h>
#include <string.h>

// This is the function exported by the compiled hello.wren
// module. We have to pass it to the runtime to load the module.
// In C++, you'd have to put extern "C" around this to
// avoid a symbol-not-found linker error.
void hello_get_globals();

void myForeignClassAlloc(WrenVM *vm);
void doForeignThing(WrenVM *vm);

WrenForeignMethodFn bindForeignMethod(
    WrenVM* vm,
    const char* module,
    const char* className,
    bool isStatic,
    const char* signature);

WrenForeignClassMethods bindForeignClass(
    WrenVM* vm, const char* module, const char* className);

static void reportError(WrenVM *vm, WrenErrorType type, const char *module,
		int line, const char *message) {
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

// This is what gets called when you write System.print.
static void writeImpl(WrenVM *ignored, const char *message) {
	// Note that the WrenVM argument here will always be NULL, as
	// global state is stored in actual global variables, so it
	// doesn't really make sense to have different VMs.
	// If you need a WrenVM here, just store the one you create in
	// a global variable.
	printf("%s", message);
}

int main() {
	// Create the VM object
	WrenConfiguration config;
	wrenInitConfiguration(&config);
	config.bindForeignMethodFn = bindForeignMethod;
	config.bindForeignClassFn = bindForeignClass;
	config.errorFn = reportError;
	config.writeFn = writeImpl;
	WrenVM *vm = wrenNewVM(&config);

	// Load the main module - this will cause all the top-level
	// code to be run, and is roughly equivilent to wrenInterpret.
	// Therefore, this will cause 'Hello, world!' to be printed
	// by our top-level print function.
	bool success = wrencInitModule(hello_get_globals);
	if (!success) {
		printf("Our module failed to load :(");
		abort();
	}

	// Call our static function, see the Wren docs for more details:
	// https://wren.io/embedding/calling-wren-from-c.html
	WrenHandle *handle = wrenMakeCallHandle(vm, "main()");
	wrenEnsureSlots(vm, 1);
	wrenGetVariable(vm, "hello", "Wren", 0);
	wrenCall(vm, handle);
}

// Foreign methods and classes work the same as they
// do in Wren. For more information, see:
// https://wren.io/embedding/calling-c-from-wren.html

typedef struct {
	int myInteger;
} MyForeignClass;

WrenForeignMethodFn bindForeignMethod(
    WrenVM* vm,
    const char* module,
    const char* className,
    bool isStatic,
    const char* signature)
{
	if (strcmp(module, "hello") != 0)
		return NULL;
	if (strcmp(className, "ForeignStuff") != 0)
		return NULL;

	if (!isStatic && strcmp(signature, "doForeignThing()") == 0)
		return doForeignThing;

	return NULL;
}

WrenForeignClassMethods bindForeignClass(
    WrenVM* vm, const char* module, const char* className)
{
	WrenForeignClassMethods methods = {NULL, NULL};

	if (strcmp(module, "hello") == 0
	 && strcmp(className, "ForeignStuff") == 0)
	{
		methods.allocate = myForeignClassAlloc;

		// There's no finaliser in this example but they are
		// of course supported, see the Wren docs for more information.
		methods.finalize = NULL;
	}

	return methods;
}

void doForeignThing(WrenVM *vm) {
	MyForeignClass *me = (MyForeignClass*) wrenGetSlotForeign(vm, 0);
	printf("Hello from foreign thing. Number: %d\n", me->myInteger);
}

void myForeignClassAlloc(WrenVM *vm) {
	void *data = wrenSetSlotNewForeign(vm, 0, 0, sizeof(MyForeignClass));
	MyForeignClass *me = (MyForeignClass*)data;

	// Read the first argument passed to the constructor
	me->myInteger = (int)wrenGetSlotDouble(vm, 1);
}
