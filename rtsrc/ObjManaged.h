//
// Created by znix on 22/07/22.
//

#pragma once

#include "Obj.h"
#include "ObjClass.h"

#include <memory>

class ObjManagedClass;
class ClassDescription; // From ClassDescription.h

/// Represents an object defined in Wren
class ObjManaged : public Obj {
  public:
	ObjManaged(ObjManagedClass *type);
	~ObjManaged();

	/// Field storage area accessed by Wren
	Value fields[];
};

/// Represents the metaclass of an object defined in Wren
class ObjManagedMetaClass : public ObjClass {
  private:
	ObjManagedMetaClass();
	~ObjManagedMetaClass();

	friend ObjManagedClass;
};

/// Represents the class of an object defined in Wren
class ObjManagedClass : public ObjClass {
  public:
	ObjManagedClass(const std::string &name, std::unique_ptr<ClassDescription> spec, ObjManagedClass *parent = nullptr);
	~ObjManagedClass();

	ObjManagedMetaClass meta;

	std::unique_ptr<ClassDescription> spec;

	/// The byte offset between the start of this class and where this class's instance fields are.
	int fieldOffset;

	/// The byte size of an ObjManaged instance representing this class, including all the fields.
	int size;
};
