//
// Created by znix on 21/07/22.
//

#include "ObjSystem.h"
#include "CoreClasses.h"
#include "Errors.h"

ObjSystem::ObjSystem() : ObjNativeClass("System", "ObjSystem") {}

void ObjSystem::WriteString_(std::string value) {
	int result = write(1 /* stdout */, value.c_str(), value.size());
	if (result != value.size()) {
		errors::wrenAbort("Could not complete write string syscall");
	}
}
