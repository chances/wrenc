//
// Created by znix on 22/07/22.
//

#pragma once

#include "Obj.h"
#include "ObjClass.h"

#include <memory>

class ObjManagedClass;
class ClassDescription; // From ClassDescription.h
class AttributePack;
class RtModule;

namespace api_interface {
class ForeignClassInterface;
}

/// Represents an object defined in Wren
class ObjManaged : public Obj {
  public:
	ObjManaged(ObjManagedClass *type);
	~ObjManaged();

	void MarkGCValues(GCMarkOps *ops) override;

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
	ObjManagedClass(const std::string &name, std::unique_ptr<ClassDescription> spec, ObjClass *parent);
	~ObjManagedClass();

	void MarkGCValues(GCMarkOps *ops) override;

	bool CanScriptSubclass() override;

	Value Attributes() override;

	ObjManagedMetaClass meta;

	std::unique_ptr<ClassDescription> spec;

	/// The module in which this class was declared.
	RtModule *declaringModule;

	/// If this is a foreign class, this contains the init/fini functions for it.
	std::unique_ptr<api_interface::ForeignClassInterface> foreignClass;

	/// The mapping between the pointers of the foreign stub methods (the compiler
	/// generates them, they call into the runtime) and the user's bound functions.
	std::unordered_map<void *, void *> foreignMethods;

	/// The byte offset between the start of this class and where this class's instance fields are.
	int fieldOffset;

	/// The byte size of an ObjManaged instance representing this class, including all the fields.
	int size;

	/// The total number of fields in this class - this is here mostly for the ObjManaged GC mark method, where
	/// it needs to know this very quickly.
	int totalFieldCount;

  private:
	void InitialiseAttributes();
	static ObjMap *BuildAttributes(const AttributePack *attributes);

	/// An instance of ClassAttributes, which is a purely-Wren type
	Value m_attributes = NULL_VAL;
};
