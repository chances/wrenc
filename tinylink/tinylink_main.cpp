//
// Created by Campbell on 14/02/2023.
//

#include "tl_common.h"

#include "PEUtil.h"
#include "tinylink_main.h"

#include <Windows.h>
#include <fmt/format.h>
#include <fstream>
#include <optional>
#include <sstream>
#include <stdio.h>

static int alignTo(int value, int alignment) {
	int overhang = value % alignment;
	if (overhang == 0)
		return value;
	value += alignment - overhang;
	return value;
}

// Parsed PE/COFF headers, used for both image (executable) files and object files.
struct PECOFF {
	uint8_t *fileBase = nullptr;
	IMAGE_FILE_HEADER *fh = nullptr;
	IMAGE_OPTIONAL_HEADER64 *oh = nullptr;
	IMAGE_SECTION_HEADER *sectionHeaders = nullptr;
	IMAGE_SYMBOL *symbolTable = nullptr;
	const char *stringTable = nullptr;
	SectionMap sections;
	ExportSection exports;
};

void *loadFile(const std::string &filename, int &size);

void parsePECOFF(PECOFF &out);

int main(int argc, char **argv) {
	// PE documentation:
	// https://learn.microsoft.com/en-gb/windows/win32/debug/pe-format

	// Make a bit of extra space for us to muck around
	int peSize;
	uint8_t *rawPE = (uint8_t *)loadFile(argv[1], peSize);
	std::vector<uint8_t> data;
	data.assign(rawPE, rawPE + peSize);
	data.resize(peSize * 2 + (1 * 1024 * 1024));
	uint8_t *pe = data.data();

	// Skip the MS DOS header, this is the offset to the PE header
	int peOffset = *(int *)(pe + 0x3c);
	IMAGE_NT_HEADERS64 *peHdr = (IMAGE_NT_HEADERS *)(pe + peOffset);

	if (peHdr->Signature != 0x4550) { // 'PE\0\0'
		fmt::print(stderr, "Invalid PE signature: {:04x}\n", peHdr->Signature);
		return 1;
	}

	PECOFF image = {
	    .fileBase = pe,
	    .fh = &peHdr->FileHeader,
	};
	parsePECOFF(image);

	if (!image.exports.IsLoaded()) {
		fmt::print(stderr, "Input executable is missing it's export directory!\n");
		exit(1);
	}

	// Load the object file to link in
	int libSize;
	uint8_t *libData = (uint8_t *)loadFile(argv[2], libSize);
	PECOFF lib = {
	    .fileBase = libData,
	    .fh = (IMAGE_FILE_HEADER *)libData,
	};
	parsePECOFF(lib);

	// TESTING FIXME
	int rva = image.exports.nameToRVA.at("wrenStandaloneMainModule");
	uint8_t *ptr = image.sections.Translate(rva);
	*(uint64_t *)ptr = 0xdeadbeef12345678;

	// FIXME disable base relocations so we don't have to
	//  write a .reloc table for now.
	image.fh->Characteristics |= IMAGE_FILE_RELOCS_STRIPPED;

	// Note: we'll just assume that there's enough space for our
	// additional section headers. The headers on the file I'm
	// testing with ends at 2E0h, and due to alignment the first
	// section doesn't start until 400h, leaving us with 288
	// bytes, or enough for seven headers.
	// Of course it's possible that other linkers might put
	// more stuff here, but it seems rather unlikely that will
	// actually ever happen.
	// (This all assumes the string table is empty, which seems
	//  reasonable since the Microsoft docs say that executables
	//  don't use them under the entry for the section header
	//  names.)

	// Copy all the sections from the object into the executable
	for (const Section &sec : lib.sections.list) {
		// Skip debug sections
		// TODO handle them later
		if (sec.name.starts_with(".debug"))
			continue;

		int physAddress = alignTo(peSize, image.oh->FileAlignment);
		int physSize = alignTo(sec.size, image.oh->FileAlignment);
		peSize = physAddress + physSize;
		memcpy(pe + physAddress, sec.data, sec.size);

		IMAGE_SECTION_HEADER *prev = &image.sectionHeaders[image.fh->NumberOfSections - 1];

		// Note: the allocated address space must be contiguous!
		// If it's not, you'll get an invalid EXE error. Pasted here from PowerShell for searches:
		//    The specified executable is not a valid application for this OS platform.
		DWORD virtualAddr = alignTo(prev->VirtualAddress + prev->Misc.VirtualSize, image.oh->SectionAlignment);

		IMAGE_SECTION_HEADER *sh = &image.sectionHeaders[image.fh->NumberOfSections++];
		sh->Misc.VirtualSize = sec.size;
		sh->VirtualAddress = virtualAddr;
		sh->SizeOfRawData = (DWORD)physSize;
		sh->PointerToRawData = (DWORD)physAddress;
		sh->PointerToRelocations = 0; // No COFF relocations in the image
		sh->PointerToLinenumbers = 0;
		sh->NumberOfRelocations = 0;
		sh->NumberOfLinenumbers = 0;
		sh->Characteristics = sec.header->Characteristics;

		// Copy in the name, truncating it if necessary. Go through an
		// intermediate buffer to deal with the trailing null.
		char name[8 + 1];
		ZeroMemory(name, sizeof(name));
		strncpy(name, sec.name.c_str(), sizeof(name));

		// Add a '2' on the end to make sure it doesn't have the same
		// name as an existing section.
		int twoPosition = 0;
		while (twoPosition < (8 - 1)) {
			if (!name[twoPosition])
				break;
			twoPosition++;
		}
		name[twoPosition] = '2';

		memcpy(sh->Name, name, 8);
	}

	// Fix up the image size attribute
	int maxVirtualAddress = 0;
	for (int i = 0; i < image.fh->NumberOfSections; i++) {
		IMAGE_SECTION_HEADER *sh = &image.sectionHeaders[i];
		int end = sh->VirtualAddress + sh->Misc.VirtualSize;
		maxVirtualAddress = std::max(maxVirtualAddress, end);
	}
	maxVirtualAddress = alignTo(maxVirtualAddress, image.oh->SectionAlignment);
	image.oh->SizeOfImage = maxVirtualAddress;
	// image.oh->SizeOfInitializedData *= 2;
	// image.oh->SizeOfInitializedData = 41472 + 0x10000;

	// Write it back out
	try {
		std::ofstream out;
		out.exceptions(std::ios::failbit | std::ios::badbit);
		out.open("link-output.exe", std::ios::binary);
		out.write((char *)pe, peSize);
	} catch (const std::fstream::failure &ex) {
		fmt::print(stderr, "Failed to write output file: {}", ex.what());
		exit(1);
	}

	return 0;
}

void parsePECOFF(PECOFF &out) {
	out.oh = (IMAGE_OPTIONAL_HEADER64 *)((uint8_t *)out.fh + sizeof(IMAGE_FILE_HEADER));

	// Read the symbol table, and pick out the string table pointer.
	// Do this before reading the sections so we have the string table
	// available.
	// Note: we can't just use the symbol table for executables (like
	// you can in, say, ELF), as it doesn't usually exist in executables.
	// Exported symbols are stored in a directory, we have to use that.
	if (out.fh->PointerToSymbolTable) {
		out.symbolTable = (IMAGE_SYMBOL *)(out.fileBase + out.fh->PointerToSymbolTable);

		// The string table immediately follows the symbol table
		out.stringTable = (const char *)&out.symbolTable[out.fh->NumberOfSymbols];

		for (int i = 0; i < out.fh->NumberOfSymbols; i++) {
			const IMAGE_SYMBOL &sym = out.symbolTable[i];

			// If this image has auxiliary symbols, this
			// is the array of them.
			const IMAGE_AUX_SYMBOL *aux = (IMAGE_AUX_SYMBOL *)&out.symbolTable[i + 1];

			std::string name;
			if (sym.N.Name.Short != 0) {
				// This symbol name is contained here
				// Null-terminate the name string
				char nameBuf[IMAGE_SIZEOF_SHORT_NAME + 1];
				ZeroMemory(nameBuf, sizeof(nameBuf));
				memcpy(nameBuf, sym.N.ShortName, IMAGE_SIZEOF_SHORT_NAME);
			} else {
				// If this is a string-table-based name, look it up.
				name = out.stringTable + sym.N.Name.Long;
			}

			// Skip over the auxiliary symbols
			i += sym.NumberOfAuxSymbols;

			fmt::print("Symbol {:3} aux={} sc={} type={} section={} value={} '{}'\n", i, sym.NumberOfAuxSymbols,
			    sym.StorageClass, sym.Type, sym.SectionNumber, (int)sym.Value, name);

			if (sym.StorageClass == IMAGE_SYM_CLASS_FILE) {
				// The following symbol is a filename
				char filenameBuf[sizeof(IMAGE_SYMBOL) + 1];
				ZeroMemory(filenameBuf, sizeof(filenameBuf));
				memcpy(filenameBuf, aux->File.Name, sizeof(IMAGE_SYMBOL));
				fmt::print("  Filename record: {}\n", filenameBuf);
			}

			// These denote section definitions.
			// if (sym.StorageClass == IMAGE_SYM_CLASS_STATIC && sym.Value == 0) {
			// 	// Don't care about anything in here.
			// 	fmt::print("  Section definition entry.\n");
			// }

			// EXTERNAL symbols are those the linker needs to resolve.
			if (sym.StorageClass == IMAGE_SYM_CLASS_EXTERNAL) {
				// TODO mark this down
			}
		}
	}

	// The sections table follows the image header.
	out.sectionHeaders = (IMAGE_SECTION_HEADER *)((uint8_t *)out.oh + out.fh->SizeOfOptionalHeader);

	for (int i = 0; i < out.fh->NumberOfSections; i++) {
		IMAGE_SECTION_HEADER *section = &out.sectionHeaders[i];

		char nameBuf[IMAGE_SIZEOF_SHORT_NAME + 1];
		ZeroMemory(nameBuf, sizeof(nameBuf));
		memcpy(nameBuf, section->Name, IMAGE_SIZEOF_SHORT_NAME);
		std::string name = nameBuf;

		// If this is a string-table-based name, look it up.
		if (nameBuf[0] == '/') {
			int offset = std::atoi(nameBuf + 1);
			name = out.stringTable + offset;
		}

		fmt::print("Section: {} at {}\n", name, section->VirtualAddress);

		out.sections.AddSection(Section{
		    .address = (int)section->VirtualAddress,
		    .size = (int)section->SizeOfRawData,
		    .data = out.fileBase + section->PointerToRawData,
		    .name = name,
		    .header = section,
		});
	}

	// Read the data directories, and pick the export directory (the first one)
	// https://learn.microsoft.com/en-gb/windows/win32/debug/pe-format#optional-header-data-directories-image-only
	std::optional<IMAGE_DATA_DIRECTORY> exportDir;
	for (int i = 0; i < out.oh->NumberOfRvaAndSizes; i++) {
		IMAGE_DATA_DIRECTORY *dir = &out.oh->DataDirectory[i];
		// fmt::print("Directory {:02d}: {:08x}\n", i, dir->VirtualAddress);

		if (i == 0) {
			exportDir = *dir;
		}
	}

	if (exportDir) {
		out.exports.Load(*exportDir, out.sections);
	}
}

void *loadFile(const std::string &filename, int &size) {
	// TODO memory-map stuff?
	std::ifstream input;
	input.exceptions(std::ios::failbit | std::ios::badbit);
	try {
		input.open(filename, std::ios::binary);

		// Quick way to read an entire stream
		std::stringstream buf;
		buf << input.rdbuf();
		std::string fileContents = buf.str();

		fmt::print("Loaded file '{}', size {}\n", filename, fileContents.size());

		// Leak the string so we can avoid copying around the file (though
		// this is performance paranoia more than anything else).
		std::string *leaky = new std::string(std::move(fileContents));
		size = leaky->size();
		return leaky->data();
	} catch (const std::fstream::failure &ex) {
		fmt::print(stderr, "Failed to load input file '{}': {}", filename.c_str(), ex.what());
		exit(1);
	}
}
