//
// Created by znix on 21/07/22.
//

#include "ObjClass.h"
#include "CoreClasses.h"
#include "hash.h"

ObjClass::ObjClass() : Obj(nullptr) {}

SignatureId ObjClass::FindSignatureId(const std::string &name) {
	static const uint64_t SIG_SEED = hashString("signature id", 0);
	uint64_t value = hashString(name, SIG_SEED);
	return SignatureId{value};
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
}

ObjNativeClass::ObjNativeClass(const std::string &name, const std::string &bindingName) {
	parentClass = &CoreClasses::Instance()->Object();
	type = &m_defaultMetaClass;
	this->name = name;

	m_defaultMetaClass.name = name;
	m_defaultMetaClass.parentClass = m_defaultMetaClass.type = &CoreClasses::Instance()->RootClass();
	m_defaultMetaClass.isMetaClass = true;

	Bind(this, bindingName, false);
	Bind(&m_defaultMetaClass, bindingName, true);
}
