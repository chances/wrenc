//
// Created by znix on 22/07/22.
//

#include "ObjManaged.h"
#include "CoreClasses.h"
#include "Errors.h"
#include "common/ClassDescription.h"

ObjManaged::ObjManaged(ObjManagedClass *type) : Obj(type) {}
ObjManaged::~ObjManaged() = default;

// Make sure the fields are at the end of the class, and there's no padding or anything like that interfering
static_assert(sizeof(ObjManaged) == offsetof(ObjManaged, fields), "ObjManaged.fields are not at the end of the class!");

ObjManagedClass::ObjManagedClass(const std::string &name, std::unique_ptr<ClassDescription> spec, ObjClass *parent)
    : spec(std::move(spec)) {

	type = &meta;
	this->name = meta.name = name;
	parentClass = parent ? parent : &CoreClasses::Instance()->Object();

	// Note the root-level ObjClass returns false, but it can be safely subclassed anyway (read the
	// comment on CanScriptSubclass to see why).
	if (!parentClass->CanScriptSubclass() && parentClass != &CoreClasses::Instance()->Object()) {
		errors::wrenAbort("Class '%s' extends C++ type '%s' which cannot be subclassed in Wren\n", name.c_str(),
		                  parentClass->name.c_str());
	}

	// If we're extending a Wren class, we need to know how many fields it uses so we can leave
	// space for them. If we're extending a C++ class, assume it has the same length as Obj (that's
	// guaranteed by CanScriptSubclass returning true).
	ObjManagedClass *managedParent = dynamic_cast<ObjManagedClass *>(parentClass);

	if (managedParent) {
		// Our fields start where the parent class ended
		fieldOffset = managedParent->size;
	} else {
		// Put the fields at the end of the class
		fieldOffset = sizeof(ObjManaged);
	}

	// Inherit our parent's methods
	functions = parentClass->functions;

	// Our size is the start of the field area plus the size taken by the fields
	size = fieldOffset + this->spec->fields.size() * sizeof(Value);
}
ObjManagedClass::~ObjManagedClass() {}

bool ObjManagedClass::CanScriptSubclass() { return true; }

ObjManagedMetaClass::ObjManagedMetaClass() {
	parentClass = type = &CoreClasses::Instance()->RootClass();
	isMetaClass = true;

	// Inherit the standard ObjClass functions
	functions = parentClass->functions;
}
ObjManagedMetaClass::~ObjManagedMetaClass() {}
