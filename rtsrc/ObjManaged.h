//
// Created by znix on 22/07/22.
//

#pragma once

#include "Obj.h"
#include "ObjClass.h"

class ObjManagedClass;

/// Represents an object defined in Wren
class ObjManaged : public Obj {
  public:
	ObjManaged(ObjManagedClass *type);
	~ObjManaged();
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
	ObjManagedClass(const std::string &name, ObjManagedClass *parent = nullptr);
	~ObjManagedClass();

	ObjManagedMetaClass meta;
};
