//
// Created by znix on 21/07/22.
//

#include "CoreClasses.h"

#include "ObjString.h"
#include "ObjSystem.h"

CoreClasses::~CoreClasses() = default;

CoreClasses::CoreClasses() {
	// Initialise the 'special three' that sit at the top of the class hierarchy
	// There's a diagram in ObjClass.h
	m_object.name = "Object";
	m_object.parentClass = nullptr;
	m_object.type = &m_objectMeta;

	// Normally we wouldn't make the metaclass ourselves, but for Object it's a bit special.
	m_objectMeta.name = m_object.name;
	m_objectMeta.isMetaClass = true;
	m_objectMeta.parentClass = &m_rootClass;
	m_objectMeta.type = &m_rootClass;

	m_rootClass.name = "Class";
	m_rootClass.isMetaClass = true;
	m_rootClass.parentClass = &m_object;
	m_rootClass.type = &m_rootClass; // The root class has itself as it's type
}

CoreClasses *CoreClasses::Instance() {
	static CoreClasses instance;
	return &instance;
}

ObjSystem &CoreClasses::System() {
	if (!m_system)
		m_system = std::make_unique<ObjSystem>();
	return *m_system;
}

ObjClass &CoreClasses::String() {
	if (!m_string) {
		// TODO make an instance of the string meta-class
		abort();
	}
	return *m_string;
}
