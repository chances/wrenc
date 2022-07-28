//
// Created by znix on 22/07/22.
//

#include "ObjManaged.h"
#include "CoreClasses.h"
#include "common/ClassDescription.h"

ObjManaged::ObjManaged(ObjManagedClass *type) : Obj(type) {}
ObjManaged::~ObjManaged() = default;

// Make sure the fields are at the end of the class, and there's no padding or anything like that interfering
static_assert(sizeof(ObjManaged) == offsetof(ObjManaged, fields), "ObjManaged.fields are not at the end of the class!");

ObjManagedClass::ObjManagedClass(const std::string &name, std::unique_ptr<ClassDescription> spec,
                                 ObjManagedClass *parent)
    : spec(std::move(spec)) {

	type = &meta;
	this->name = meta.name = name;
	parentClass = parent ? parent : &CoreClasses::Instance()->Object();

	if (parent) {
		// Our fields start where the parent class ended
		fieldOffset = parent->size;
	} else {
		// Put the fields at the end of the class
		fieldOffset = sizeof(ObjManaged);
	}

	// Our size is the start of the field area plus the size taken by the fields
	size = fieldOffset + this->spec->fields.size() * sizeof(Value);
}
ObjManagedClass::~ObjManagedClass() {}

ObjManagedMetaClass::ObjManagedMetaClass() {
	parentClass = type = &CoreClasses::Instance()->RootClass();
	isMetaClass = true;
}
ObjManagedMetaClass::~ObjManagedMetaClass() {}
