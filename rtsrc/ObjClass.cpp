//
// Created by znix on 21/07/22.
//

#include "ObjClass.h"
#include "CoreClasses.h"
#include "Errors.h"
#include "binding_utils.h" // Useful for the manually-written ObjClass getters

#include <inttypes.h>
#include <unordered_map>

static std::unordered_map<uint64_t, std::string> signatureNames;
static std::vector<ObjNativeClass *> nativeClasses;

ObjClass::~ObjClass() = default;
ObjClass::ObjClass() : Obj(nullptr) {}

SignatureId ObjClass::FindSignatureId(const std::string &name) {
	SignatureId id = hash_util::findSignatureId(name);
	signatureNames[id] = name;
	return id;
}

std::string ObjClass::LookupSignatureFromId(SignatureId id, bool allowUnknown) {
	auto iter = signatureNames.find(id);
	if (iter != signatureNames.end())
		return iter->second;

	// Return the signature as a hex value
	char buff[32];
	snprintf(buff, sizeof(buff), "!UKN-0x%016" PRIx64, id.id);
	return buff;
}

FunctionTable::Entry *ObjClass::LookupMethod(SignatureId signature) {
	int startIndex = signature % FunctionTable::NUM_ENTRIES;
	int index = startIndex;
	while (true) {
		FunctionTable::Entry *entry = &functions.entries[index];

		// Check if this entry matches - if two entries have the same signature modulo the function
		// table size, they'll be placed in an arbitrary sequence.
		if (entry->signature == signature) {
			return entry;
		}

		// If there's an empty space here, we haven't found it so stop
		if (entry->func == nullptr)
			return nullptr;

		// Move on to the next one
		index++;

		// Wrap around
		if (index == FunctionTable::NUM_ENTRIES)
			index = 0;

		// Should never get all the way around without finding an empty space
		if (index == startIndex) {
			errors::wrenAbort("Function table in class %s full!", type->name.c_str());
		}
	}
}

void ObjClass::AddFunction(const std::string &signature, void *funcPtr, bool shouldOverride) {
	SignatureId id = FindSignatureId(signature);

	// Insert the function into the function table, then point definedFunctions to that entry
	// This function returns the slot of a function that already exists, so we can override methods with it.
	FunctionTable::Entry *entry = FindFunctionTableEntry(id);

	if (!shouldOverride && entry->func != nullptr)
		return;

	entry->func = funcPtr;
	entry->signature = id;
	definedFunctions.push_back(entry);
}

FunctionTable::Entry *ObjClass::FindFunctionTableEntry(SignatureId signature) {
	// Put the function in the first free slot
	uint64_t idx = signature % FunctionTable::NUM_ENTRIES;
	while (true) {
		// If there is no matching entry, pick the first empty slot.
		if (!functions.entries[idx].func)
			break;

		// If there is a matching entry, pick it.
		if (functions.entries[idx].signature == signature)
			break;

		// TODO wrap around to the start
		idx++;
		if (idx >= FunctionTable::NUM_ENTRIES) {
			errors::wrenAbort("Function table overflow for class %s", name.c_str());
		}
	}

	return &functions.entries[idx];
}

bool ObjClass::CanScriptSubclass() { return false; }

bool ObjClass::InheritsMethods() { return true; }

std::string ObjClass::Name() const { return name; }
ObjClass *ObjClass::Supertype() const { return parentClass; }
std::string ObjClass::ToString() const { return name; }
Value ObjClass::Attributes() { return NULL_VAL; }

bool ObjClass::Extends(ObjClass *other) {
	// Walk through the type hierarchy and see if we are or extend the specified class
	ObjClass *iter = this;
	while (iter) {
		if (iter == other)
			return true;
		iter = iter->parentClass;
	}
	return false;
}

ObjNativeClass::ObjNativeClass(const std::string &name, const std::string &bindingName) {
	type = &m_defaultMetaClass;
	this->name = name;

	m_defaultMetaClass.name = name;
	m_defaultMetaClass.parentClass = m_defaultMetaClass.type = &CoreClasses::Instance()->RootClass();
	m_defaultMetaClass.isMetaClass = true;

	// Inherit ObjClass's static methods
	m_defaultMetaClass.functions = m_defaultMetaClass.parentClass->functions;

	Bind(this, bindingName, false);
	Bind(&m_defaultMetaClass, bindingName, true);

	nativeClasses.push_back(this);
}

void ObjNativeClass::FinaliseSetup() {
	// Guard against running twice
	static bool initialised = false;
	if (initialised) {
		errors::wrenAbort("Cannot run ObjNativeClass::FinaliseSetup twice!");
	}
	initialised = true;

	for (ObjNativeClass *cls : nativeClasses) {
		if (!cls->InheritsMethods())
			continue;

		for (ObjClass *parent = cls->parentClass; parent; parent = parent->parentClass) {
			for (FunctionTable::Entry *superEntry : parent->definedFunctions) {
				// If this function hasn't already been defined, then copy in it's definition
				// We do this starting from the parent and going in the direction of grandparents, so this will
				// locate the best match. If A extends B and both need a method copied like this it doesn't
				// matter what order they're finalised in, as A will either copy from B or use the same search
				// logic to find the method from (eg) C instead.
				FunctionTable::Entry *clsEntry = cls->FindFunctionTableEntry(superEntry->signature);
				if (clsEntry->func == nullptr) {
					*clsEntry = *superEntry;
				}
			}
		}
	}
}
