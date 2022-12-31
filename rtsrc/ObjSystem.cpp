//
// Created by znix on 21/07/22.
//

#include "ObjSystem.h"
#include "CoreClasses.h"
#include "Errors.h"
#include "WrenRuntime.h"

class ObjSystemClass : public ObjNativeClass {
  public:
	ObjSystemClass() : ObjNativeClass("System", "ObjSystem") {}
};

ObjSystem::ObjSystem() : Obj(Class()) {
	printf("Cannot instantiate System!\n");
	abort();
}

ObjClass *ObjSystem::Class() {
	static ObjSystemClass sysClass;
	return &sysClass;
}

void ObjSystem::WriteString_(std::string value) {
	int result = write(1 /* stdout */, value.c_str(), value.size());
	if (result != value.size()) {
		errors::wrenAbort("Could not complete write string syscall");
	}
}

void ObjSystem::Gc() { WrenRuntime::Instance().RunGC(); }

void ObjSystem::MarkGCValues(GCMarkOps *ops) {
	// This class doesn't have any constructors, so no instances of it should exist.
	abort();
}

double ObjSystem::Clock() {
	// This is the same implementation as Wren, so it should be accurate.
	return (double)clock() / CLOCKS_PER_SEC;
}
