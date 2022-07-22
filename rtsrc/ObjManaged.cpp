//
// Created by znix on 22/07/22.
//

#include "ObjManaged.h"
#include "CoreClasses.h"

ObjManaged::ObjManaged(ObjManagedClass *type) : Obj(type) {}
ObjManaged::~ObjManaged() = default;

ObjManagedClass::ObjManagedClass(const std::string &name, ObjManagedClass *parent) {
	type = &meta;
	this->name = meta.name = name;
	parentClass = parent ? parent : &CoreClasses::Instance()->Object();
}
ObjManagedClass::~ObjManagedClass() {}

ObjManagedMetaClass::ObjManagedMetaClass() {
	parentClass = type = &CoreClasses::Instance()->RootClass();
	isMetaClass = true;
}
ObjManagedMetaClass::~ObjManagedMetaClass() {}
