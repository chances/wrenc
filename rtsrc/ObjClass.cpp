//
// Created by znix on 21/07/22.
//

#include "ObjClass.h"
#include "CoreClasses.h"

#include <inttypes.h>
#include <unordered_map>

static std::unordered_map<uint64_t, std::string> signatureNames;

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
			printf("Function table in class %s full!", type->name.c_str());
			abort();
		}
	}
}

void ObjClass::AddFunction(const std::string &signature, void *funcPtr) {
	SignatureId id = FindSignatureId(signature);

	// Put the function in the first free slot
	uint64_t idx = id % FunctionTable::NUM_ENTRIES;
	while (true) {
		if (!functions.entries[idx].func)
			break;

		// TODO do this properly
		idx++;
		if (idx >= FunctionTable::NUM_ENTRIES)
			abort();
	}

	FunctionTable::Entry &entry = functions.entries[idx];
	entry.func = funcPtr;
	entry.signature = id;

	definedFunctions.push_back(&entry);
}

ObjNativeClass::ObjNativeClass(const std::string &name, const std::string &bindingName, ObjClass *parent,
                               bool inheritParentMethods) {
	parentClass = parent ? parent : &CoreClasses::Instance()->Object();
	type = &m_defaultMetaClass;
	this->name = name;

	m_defaultMetaClass.name = name;
	m_defaultMetaClass.parentClass = m_defaultMetaClass.type = &CoreClasses::Instance()->RootClass();
	m_defaultMetaClass.isMetaClass = true;

	// If we should inherit the methods from our parent, then copy them over.
	// ObjClass doesn't automatically bind it's methods though, so in that case initialise them ourselves
	if (inheritParentMethods) {
		if (parentClass == &CoreClasses::Instance()->Object()) {
			Bind(this, "Obj", false);
		} else {
			functions = parentClass->functions;
		}
	}

	Bind(this, bindingName, false);
	Bind(&m_defaultMetaClass, bindingName, true);
}
