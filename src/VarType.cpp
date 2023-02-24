//
// Created by znix on 23/02/23.
//

#include "VarType.h"
#include "ArenaAllocator.h"

VarType::VarType() {}
VarType::VarType(Type type) : type(type) {}

std::string VarType::ToString() const {
	std::string result;

	switch (type) {
	case NULL_TYPE:
		result = "null";
		break;
	case NUM:
		result = "num";
		break;
	case OBJECT:
		result = "obj";
		break;
	case OBJECT_SYSTEM:
		result = "obj_sys:";
		result += systemClassName;
		break;
	}

	return result;
}

VarType *VarType::SysClass(ArenaAllocator *allocator, const char *systemClassName) {
	VarType *type = allocator->New<VarType>(OBJECT_SYSTEM);
	type->systemClassName = systemClassName;
	return type;
}
