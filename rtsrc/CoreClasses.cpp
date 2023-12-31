//
// Created by znix on 21/07/22.
//

#include "CoreClasses.h"

#include "ObjString.h"

CoreClasses::~CoreClasses() = default;

CoreClasses::CoreClasses() {
	// Initialise the 'special three' that sit at the top of the class hierarchy
	// There's a diagram in ObjClass.h
	m_object.name = "Object";
	m_object.parentClass = nullptr;
	m_object.type = &m_objectMeta;

	// Normally we wouldn't make the metaclass ourselves, but for Object it's a bit special.
	m_objectMeta.name = m_object.GetDefaultMetaclassName();
	m_objectMeta.isMetaClass = true;
	m_objectMeta.parentClass = &m_rootClass;
	m_objectMeta.type = &m_rootClass;

	m_rootClass.name = "Class";
	m_rootClass.isMetaClass = true;
	m_rootClass.parentClass = &m_object;
	m_rootClass.type = &m_rootClass; // The root class has itself as it's type

	// Bind the Obj and ObjClass functions
	ObjClass::Bind(&m_object, "Obj", false);

	m_rootClass.functions = m_rootClass.parentClass->functions;
	ObjClass::Bind(&m_rootClass, "ObjClass", false);

	// Bind the metaclass last, since it inherits it's methods from ObjClass.
	// Without this, you couldn't call 'Object.name' as that's inherited from ObjClass.
	m_objectMeta.functions = m_objectMeta.parentClass->functions;
	ObjClass::Bind(&m_objectMeta, "Obj", true);
}

CoreClasses *CoreClasses::Instance() {
	static CoreClasses instance;
	return &instance;
}
