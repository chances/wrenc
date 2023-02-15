//
// Created by Campbell on 14/02/2023.
//

#include "common.h"

#include "PEUtil.h"

#include <fmt/format.h>

void SectionMap::AddSection(Section section) {
	if (DoesCollide(section)) {
		fmt::print(stderr, "Cannot add section {} size {} to map - collision\n", section.address, section.size);
		abort();
	}

	list.push_back(section);

	// Object files put all their virtual addresses at zero, because we're
	// supposed to pick that.
	if (section.address == 0)
		return;

	int address = section.address;
	sections[address] = std::move(section);
}

uint8_t *SectionMap::Translate(int address) const {
	// Find the first section after this address - this
	// won't contain it, but we need to do it this way since
	// there isn't a method to get the last section before
	// an address.
	auto iter = sections.upper_bound(address);

	// If this address is before the first section, it's
	// obviously not mapped.
	if (iter == sections.begin()) {
		return nullptr;
	}

	// Step back to the previous section - if any contain
	// the address we after, it'll be this one.
	iter--;
	const Section &sec = iter->second;

	// Check if this address isn't inside the section.
	if (!(sec.address <= address && address < sec.address + sec.size)) {
		return nullptr;
	}

	// Found it! Now build the pointer.
	int offset = address - sec.address;
	return sec.data + offset;
}

bool SectionMap::DoesCollide(const Section &sec) const {
	// Finds the first section whose address is greater than our argument
	auto iter = sections.upper_bound(sec.address);

	// Unless this section is greater than all those already
	// in the map, check if it collides with the next one.
	if (iter != sections.end()) {
		if (sec.address + sec.size > iter->second.address) {
			return true;
		}
	}

	// Unless this section comes before any others, check if
	// it overlaps with the previous one.
	if (iter != sections.begin()) {
		iter--;

		if (iter->second.address + iter->second.size > sec.address) {
			return true;
		}
	}

	// No collision
	return false;
}

bool ExportSection::IsLoaded() const { return exportDir != nullptr; }

void ExportSection::Load(const IMAGE_DATA_DIRECTORY &dir, const SectionMap &sections) {
	// Do the RVA->pointer lookup
	exportDir = (IMAGE_EXPORT_DIRECTORY *)sections.Translate(dir.VirtualAddress);

	if (!exportDir) {
		fmt::print(stderr, "Input executable has an invalid export directory mapping!\n");
		exit(1);
	}

	// Load the export address table
	struct AddrEntry {
		int rva;
		int forwarder;
	};
	AddrEntry *addrTable = (AddrEntry *)sections.Translate(exportDir->AddressOfFunctions);
	for (int i = 0; i < exportDir->NumberOfFunctions; i++) {
		// Assume nothing is forwarded
		addresses.push_back(addrTable[i].rva);
	}

	// Load the name table
	int *nameTable = (int *)sections.Translate(exportDir->AddressOfNames);
	for (int i = 0; i < exportDir->NumberOfNames; i++) {
		const char *name = (const char *)sections.Translate(nameTable[i]);
		names.push_back(name);
	}

	// Load the name ordinal table
	// See the note about ordinal biasing (TL;DR we can basically ignore it):
	// https://learn.microsoft.com/en-gb/windows/win32/debug/pe-format#export-ordinal-table
	int16_t *nameOrdinalList = (int16_t *)sections.Translate(exportDir->AddressOfNameOrdinals);
	nameOrdinals.assign(nameOrdinalList, nameOrdinalList + exportDir->NumberOfNames);

	// Now combine them together into a name-to-RVA mapping
	for (int i = 0; i < names.size(); i++) {
		int ordinal = nameOrdinals.at(i);
		int rva = addresses.at(ordinal);
		nameToRVA[names.at(i)] = rva;
	}
}
