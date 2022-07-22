//
// Created by znix on 21/07/22.
//

#include "ObjSystem.h"
#include "CoreClasses.h"

ObjSystem::ObjSystem() : ObjNativeClass("System", "ObjSystem") {}

void ObjSystem::Print(Value value) {
	std::string str = Obj::ToString(value);
	printf("System print: %s\n", str.c_str());
}
