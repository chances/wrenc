//
// Created by Campbell on 14/02/2023.
//

#pragma once

#include "tl_common.h"

#include <map>
#include <string>
#include <vector>

struct Section {
	int id = -1;
	int address = 0;
	int size = 0;
	uint8_t *data = nullptr;
	std::string name;
	IMAGE_SECTION_HEADER *header = nullptr;
};

// Represents the memory-map of an image.
class SectionMap {
  public:
	std::map<int, Section> sections;

	/// The list of all sections, even if they don't
	/// have a base address.
	std::vector<Section> list;

	void AddSection(Section section);

	uint8_t *Translate(int address) const;

	bool DoesCollide(const Section &sec) const;
};

// Loads all the various parts of the export table.
class ExportSection {
  public:
	IMAGE_EXPORT_DIRECTORY *exportDir = nullptr;
	std::vector<int> addresses;
	std::vector<std::string> names;
	std::vector<int> nameOrdinals;

	std::map<std::string, int> nameToRVA;

	bool IsLoaded() const;

	void Load(const IMAGE_DATA_DIRECTORY &dir, const SectionMap &sections);
};

class ImportSection {
  public:
	const IMAGE_IMPORT_DESCRIPTOR *importDir = nullptr;
	const IMAGE_IMPORT_DESCRIPTOR *wrenImport = nullptr;

	std::map<std::string, int> importNameRVAs;

	void Load(const IMAGE_DATA_DIRECTORY &dir, const SectionMap &sections);
};
