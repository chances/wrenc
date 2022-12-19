//
// Created by znix on 22/07/22.
//

#include "ObjManaged.h"
#include "CoreClasses.h"
#include "Errors.h"
#include "ObjBool.h"
#include "ObjList.h"
#include "ObjMap.h"
#include "ObjString.h"
#include "WrenRuntime.h"
#include "common/ClassDescription.h"

ObjManaged::ObjManaged(ObjManagedClass *type) : Obj(type) {}
ObjManaged::~ObjManaged() = default;

// Make sure the fields are at the end of the class, and there's no padding or anything like that interfering
static_assert(sizeof(ObjManaged) == offsetof(ObjManaged, fields), "ObjManaged.fields are not at the end of the class!");

ObjManagedClass::ObjManagedClass(const std::string &name, std::unique_ptr<ClassDescription> spec, ObjClass *parent)
    : spec(std::move(spec)) {

	type = &meta;
	this->name = name;
	meta.name = name + " metaclass";
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

	InitialiseAttributes();
}
ObjManagedClass::~ObjManagedClass() {}

bool ObjManagedClass::CanScriptSubclass() { return true; }

Value ObjManagedClass::Attributes() { return m_attributes; }

void ObjManagedClass::InitialiseAttributes() {
	if (spec->attributes.empty())
		return;

	// Sort all the attributes from groups into packs
	AttributePack classAttrs;
	std::map<std::string, AttributePack> methodAttrs;
	for (const ClassDescription::AttributeGroup &group : spec->attributes) {
		// Find the correct pack for this group
		AttributePack *pack;
		if (group.method == -1) {
			pack = &classAttrs;
		} else {
			ClassDescription::MethodDecl &method = spec->methods.at(group.method);
			std::string name = method.name;
			// TODO foreign prefix
			if (method.isStatic)
				name = "static " + name;

			pack = &methodAttrs[name];
		}

		for (const auto &[name, attr] : group.attributes) {
			AttrKey key = {.group = group.name, .name = name};
			pack->attributes[key].emplace_back(attr);
		}
	}

	ObjMap *classAttrsMap = BuildAttributes(&classAttrs);

	ObjMap *methodsMap = methodAttrs.empty() ? nullptr : ObjMap::New();
	for (const auto &[name, pack] : methodAttrs) {
		ObjString *nameStr = ObjString::New(name);
		ObjMap *methodAttrMap = BuildAttributes(&pack);
		methodsMap->OperatorSubscriptSet(encode_object(nameStr), encode_object(methodAttrMap));
	}

	// Lookup the constructor that's defined in Wren
	typedef Value (*ctor_t)(Value receiver, Value attributes, Value methods);
	Value attrsClass = WrenRuntime::Instance().GetCoreGlobal("ClassAttributes");
	ObjClass *cls = dynamic_cast<ObjClass *>(get_object_value(attrsClass));
	ctor_t ctor = (ctor_t)cls->type->LookupMethod(FindSignatureId("new(_,_)"))->func;

	// Create the ClassAttributes instance
	m_attributes = ctor(attrsClass, encode_object(classAttrsMap), encode_object(methodsMap));
}

ObjMap *ObjManagedClass::BuildAttributes(const AttributePack *attributes) {
	// The map should be null if there's nothing in it, so we lazy-initialise it
	ObjMap *map = attributes->attributes.empty() ? nullptr : WrenRuntime::Instance().New<ObjMap>();

	std::map<std::string, ObjMap *> groups;

	for (const auto &[key, contents] : attributes->attributes) {
		if (!groups.contains(key.group)) {
			ObjMap *groupMap = ObjMap::New();
			groups[key.group] = groupMap;
			// An empty string means a null group
			Value groupKey = key.group.empty() ? NULL_VAL : encode_object(ObjString::New(key.group));
			map->OperatorSubscriptSet(groupKey, encode_object(groupMap));
		}
		ObjMap *groupMap = groups.at(key.group);

		ObjList *nameList = ObjList::New();
		for (const AttrContent &attr : contents) {
			Value value;
			if (attr.str) {
				value = encode_object(ObjString::New(attr.str.value()));
			} else if (attr.boolean) {
				value = encode_object(ObjBool::Get(attr.boolean.value()));
			} else {
				value = attr.value;
			}

			nameList->Add(value);
		}

		groupMap->OperatorSubscriptSet(encode_object(ObjString::New(key.name)), encode_object(nameList));
	}

	return map;
}

ObjManagedMetaClass::ObjManagedMetaClass() {
	parentClass = type = &CoreClasses::Instance()->RootClass();
	isMetaClass = true;

	// Inherit the standard ObjClass functions
	functions = parentClass->functions;
}
ObjManagedMetaClass::~ObjManagedMetaClass() {}
