//
// Created by Campbell on 14/02/2023.
//

#include "tl_common.h"

#include "PEUtil.h"
#include "tinylink_main.h"

#include <Windows.h>
#include <algorithm>
#include <assert.h>
#include <fmt/format.h>
#include <fstream>
#include <optional>
#include <sstream>
#include <stdio.h>
#include <ya_getopt.h>

bool verbose = false;

// Keep track of whether we've hit any errors that justify exiting with a
// non-zero return code. We'll still continue processing the file to make
// debugging easier, but this should bubble up making the compiler fail
// and eventually warning the user via whatever UI they're using for running
// the compiler.
static bool hitFatalError = false;

static int alignTo(int value, int alignment) {
	int overhang = value % alignment;
	if (overhang == 0)
		return value;
	value += alignment - overhang;
	return value;
}

static void ensureAlignment(std::vector<uint8_t> &data, int alignment) { data.resize(alignTo(data.size(), alignment)); }

struct GeneratedSection;
struct Symbol;
struct IntermediateState;

// Parsed PE/COFF headers, used for both image (executable) files and object files.
struct PECOFF {
	uint8_t *fileBase = nullptr;
	IMAGE_FILE_HEADER *fh = nullptr;
	IMAGE_OPTIONAL_HEADER64 *oh = nullptr;
	IMAGE_SECTION_HEADER *sectionHeaders = nullptr;
	IMAGE_SYMBOL *symbolTable = nullptr;
	const char *stringTable = nullptr;
	SectionMap sections;
	ImportSection imports;
	ExportSection exports;
	std::optional<IMAGE_DATA_DIRECTORY *> exceptionsDir;
	std::vector<std::string> symbolNames;
};

struct Symbol {
	// The section where the symbol lives, or null for an external symbol.
	GeneratedSection *section = nullptr;
	std::string name;

	int originalIndex; // The index in the original PE file.

	// From IMAGE_SYMBOL
	DWORD value = 0;
	BYTE storageClass = 0;

	// If true, this is a relocation that's part of the import stubs.
	// In this case, directly pass through the address from the import table.
	bool isImportReloc = false;

	int RVA(IntermediateState &state, PECOFF &image) const;
};

// Our intermediate relocation form, used when merging objects.
struct Relocation {
	int symbolId = 0; // Index in IntermediateState.symbols.
	int offset = 0;   // Address of the relocation from the generated section start.
	WORD type = 0;    // From IMAGE_RELOCATION
};

// https://learn.microsoft.com/en-us/windows/win32/debug/pe-format#the-pdata-section
struct ExceptionRecord {
	unsigned int beginRVA = 0;
	unsigned int endRVA = 0;
	unsigned int unwindRVA = 0;
};

// Represents a section that's being generated by combining object files.
struct GeneratedSection {
	int id = -1; // Index in orderedSections.
	std::string name;
	DWORD characteristics = 0;
	std::vector<uint8_t> data;
	std::vector<Relocation> relocations;

	// If this is an uninitialised section, this tracks how
	// much data is present.
	int uninitialisedSize = 0;

	// Assigned when these are copied to the output
	IMAGE_SECTION_HEADER *sectionHeader = nullptr;
	int virtualAddress = -1;

	bool IsUninitialised() { return (characteristics & IMAGE_SCN_CNT_UNINITIALIZED_DATA) != 0; }

	template <typename T> void Append(const T &value) {
		uint8_t *valuePtr = (uint8_t *)&value;
		data.insert(data.end(), valuePtr, valuePtr + sizeof(T));
	}
};

// The data that's used to combine one or more images before
// it gets appended to the executable.
struct IntermediateState {
	std::vector<Symbol> symbols;
	std::map<std::string, std::unique_ptr<GeneratedSection>> newSections;
	std::vector<GeneratedSection *> orderedSections;
	std::optional<int /* symbol index */> mainModuleGlobals;

	std::map<std::string, int /* symbol index */> importStubs;
	std::map<std::string, int /* symbol index */> moduleGetGlobals;
};

void *loadFile(const std::string &filename, int &size);

void parsePECOFF(PECOFF &out);

void appendObject(IntermediateState &state, PECOFF &lib);

void createDllImportStubs(IntermediateState &state, const PECOFF &image);

void rewriteExceptions(PECOFF &image, std::vector<ExceptionRecord> &exceptions, GeneratedSection *pdata);

int createExportsTable(IntermediateState &state, int &outSize);

int main(int argc, char **argv) {
	const char *exeInputFile = nullptr;
	const char *outputFile = nullptr;
	std::vector<const char *> objectFiles;

	bool hitInvalidArg = false;
	bool dllMode = false;

	while (true) {
		int arg = getopt(argc, argv, "e:o:hvd");

		// Out of arguments?
		if (arg == -1)
			break;

		switch (arg) {
		case 'd': {
			dllMode = true;
			break;
		}
		case 'e': {
			exeInputFile = optarg;
			break;
		}
		case 'o': {
			outputFile = optarg;
			break;
		}
		case 'v': {
			verbose = true;
			break;
		}
		case 'h': {
			// Print help immediately and do nothing else, so you can
			// put it in with invalid commands and get help.
			printf("Usage: %s [-hvd] -e exe -o output objects...\n", argv[0]);
			printf("\t-h        Show this help message\n");
			printf("\t-v        Print debugging information\n");
			printf("\t-e exe    Set the path to the input EXE file\n");
			printf("\t-d        DLL mode - read and write a DLL\n");
			return 0;
		}
		case '?': {
			// getopt already printed an error, finish parsing the arguments then exit.
			hitInvalidArg = true;
			break;
		}
		default:
			// Invalid options are handled by getopt.
			fmt::print(stderr, "Found unhandled argument {}.\n", arg);
			abort();
		}
	}

	if (hitInvalidArg)
		return 1;

	for (int i = optind; i < argc; i++) {
		objectFiles.push_back(argv[i]);
	}

	if (!exeInputFile) {
		fmt::print(stderr, "Missing EXE input file (use -e)! See -h for usage information.\n");
		return 1;
	}
	if (!outputFile) {
		fmt::print(stderr, "Missing output file! See -h for usage information.\n");
		return 1;
	}

	if (objectFiles.empty()) {
		fmt::print(stderr, "No input object files specified! See -h for usage information.\n");
		return 1;
	}

	// PE documentation:
	// https://learn.microsoft.com/en-gb/windows/win32/debug/pe-format

	// Make a bit of extra space for us to muck around
	int peSize;
	uint8_t *rawPE = (uint8_t *)loadFile(exeInputFile, peSize);
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

	IntermediateState state;

	// Load all the the object files to link in
	for (const std::string &file : objectFiles) {
		int libSize;
		uint8_t *libData = (uint8_t *)loadFile(file, libSize);
		PECOFF lib = {
		    .fileBase = libData,
		    .fh = (IMAGE_FILE_HEADER *)libData,
		};
		parsePECOFF(lib);

		appendObject(state, lib);
	}

	// Create little mini-functions that jump to a DLL-imported function.
	createDllImportStubs(state, image);

	// If we're exporting a DLL, create a new exports table
	int exportTableSym = -1;
	int exportDirSize = -1;
	if (dllMode) {
		exportTableSym = createExportsTable(state, exportDirSize);
	}

	// We need to add in our exception data, so make space in the
	// new .pdata section for that. We'll copy it in after relocations
	// since we need to sort the RVAs.
	GeneratedSection *pdata = state.newSections.at(".pdata").get();
	std::vector<ExceptionRecord> exceptions;
	if (image.exceptionsDir) {
		IMAGE_DATA_DIRECTORY *exDir = image.exceptionsDir.value();

		// Actually parse out the exceptions - there's no real reason
		// to do it later, and we get the exact size this way.
		const auto *records = (IMAGE_AMD64_RUNTIME_FUNCTION_ENTRY *)image.sections.Translate(exDir->VirtualAddress);
		for (int i = 0; i < exDir->Size / 12; i++) {
			exceptions.push_back(ExceptionRecord{
			    .beginRVA = records->BeginAddress,
			    .endRVA = records->EndAddress,
			    .unwindRVA = records->UnwindInfoAddress,
			});

			records++;
		}

		pdata->data.resize(pdata->data.size() + exceptions.size() * sizeof(IMAGE_AMD64_RUNTIME_FUNCTION_ENTRY));
	}

	// Copy all the sections from the object into the executable
	for (GeneratedSection *secPtr : state.orderedSections) {
		GeneratedSection &sec = *secPtr;

		// Skip debug sections
		// TODO handle them later
		if (sec.name.starts_with(".debug"))
			continue;

		// Don't copy out empty sections! That corrupts the EXE with the same
		// error as gaps in the virtual memory.
		if (sec.data.empty() && sec.uninitialisedSize == 0)
			continue;

		int physAddress = alignTo(peSize, image.oh->FileAlignment);
		int physSize = alignTo(sec.data.size(), image.oh->FileAlignment);
		peSize = physAddress + physSize;
		memcpy(pe + physAddress, sec.data.data(), sec.data.size());

		IMAGE_SECTION_HEADER *prev = &image.sectionHeaders[image.fh->NumberOfSections - 1];

		// Note: the allocated address space must be contiguous!
		// If it's not, you'll get an invalid EXE error. Pasted here from PowerShell for searches:
		//    The specified executable is not a valid application for this OS platform.
		sec.virtualAddress = alignTo(prev->VirtualAddress + prev->Misc.VirtualSize, image.oh->SectionAlignment);

		IMAGE_SECTION_HEADER *sh = &image.sectionHeaders[image.fh->NumberOfSections++];
		sec.sectionHeader = sh;
		sh->Misc.VirtualSize = sec.data.size() + sec.uninitialisedSize;
		sh->VirtualAddress = sec.virtualAddress;
		sh->SizeOfRawData = (DWORD)physSize;
		sh->PointerToRawData = (DWORD)physAddress;
		sh->PointerToRelocations = 0; // No COFF relocations in the image
		sh->PointerToLinenumbers = 0;
		sh->NumberOfRelocations = 0;
		sh->NumberOfLinenumbers = 0;
		sh->Characteristics = sec.characteristics;

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

	// Apply all the section relocations
	for (GeneratedSection *sec : state.orderedSections) {
		if (verbose)
			printf("relocating %s\n", sec->name.c_str());

		// If this section wasn't placed, don't try doing anything with it's relocations
		if (!sec->sectionHeader)
			continue;

		// Apply the section relocations
		// FIXME implement IMAGE_SCN_LNK_NRELOC_OVFL
		for (const Relocation &reloc : sec->relocations) {
			const Symbol &sym = state.symbols.at(reloc.symbolId);

			if (verbose) {
				printf("relocation t=%d at %08x against %d - %s\n", reloc.type, (int)reloc.offset, sym.originalIndex,
				    sym.name.c_str());
			}

			int targetRVA = sym.RVA(state, image);

			int64_t targetVA = targetRVA + image.oh->ImageBase;

			uint8_t *toEdit = pe + sec->sectionHeader->PointerToRawData + reloc.offset;

			int relocRVA = sec->virtualAddress + reloc.offset;

			// I think adding the calculated values - rather than overwriting
			// them - is correct.

			if (reloc.type == IMAGE_REL_AMD64_ADDR64) {
				*(int64_t *)(toEdit) += targetVA;
			} else if (reloc.type == IMAGE_REL_AMD64_REL32) {
				// +4 since this is relative to the address of the first byte
				// *after* the value we're relocating.
				*(int32_t *)(toEdit) += targetRVA - (relocRVA + 4);
			} else if (reloc.type == IMAGE_REL_AMD64_ADDR32NB) {
				// It sounds like this is just the plain RVA? The MSDN docs
				// aren't especially clear.
				// I can't find any documentation for it, but an LLVM Phabricator
				// patch implements it and it's easy to see what it does:
				// https://reviews.llvm.org/D30709
				*(int32_t *)(toEdit) += targetRVA;
			} else {
				printf("\tWARN: Unknown reloc type %d for symbol '%s'\n", reloc.type, sym.name.c_str());
				hitFatalError = true;
			}
		}
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

	rewriteExceptions(image, exceptions, pdata);

	if (dllMode) {
		// Set up the DLL export section
		Symbol &exportTable = state.symbols.at(exportTableSym);
		image.oh->DataDirectory[0] = {
		    .VirtualAddress = (DWORD)exportTable.RVA(state, image),
		    .Size = (DWORD)exportDirSize,
		};
	} else {
		if (!image.exports.IsLoaded()) {
			fmt::print(stderr, "Input executable is missing it's export directory!\n");
			exit(1);
		}

		// Link up the main module pointer
		const Symbol &getGlobalsSym = state.symbols.at(state.mainModuleGlobals.value());
		int getGlobalsFileOffset = getGlobalsSym.section->sectionHeader->PointerToRawData + getGlobalsSym.value;
		uint64_t getGlobalsValue = *(uint64_t *)(pe + getGlobalsFileOffset);
		int rva = image.exports.nameToRVA.at("wrenStandaloneMainModule");
		uint8_t *ptr = image.sections.Translate(rva);
		*(uint64_t *)ptr = getGlobalsValue;
	}

	// Write it back out
	try {
		std::ofstream out;
		out.exceptions(std::ios::failbit | std::ios::badbit);
		out.open(outputFile, std::ios::binary);
		out.write((char *)pe, peSize);
	} catch (const std::fstream::failure &ex) {
		fmt::print(stderr, "Failed to write output file: {}", ex.what());
		exit(1);
	}

	if (hitFatalError) {
		fmt::print(stderr, "Hit error while processing the file, exiting with non-zero status code.\n");
		return 1;
	}

	// Make it easy to see when it crashed.
	if (verbose)
		fmt::print("Linking complete.\n");

	return 0;
}

int Symbol::RVA(IntermediateState &state, PECOFF &image) const {
	if (isImportReloc) {
		// This is the relocation we generate in createDllImportStubs for
		// dealing with DLL imports.
		return image.imports.importNameRVAs.at(name);
	}
	if (storageClass == IMAGE_SYM_CLASS_STATIC || storageClass == IMAGE_SYM_CLASS_EXTERNAL) {
		if (section != nullptr) {
			// The section must have been written out.
			assert(section->virtualAddress != -1);

			// This relocation is against a section that was in the object file.
			return section->virtualAddress + value;
		}

		// This relocation is against a symbol that's not in the object
		// file, and is imported from another object file or the runtime.

		// Check if it's imported from the runtime.
		auto iter = state.importStubs.find(name);
		if (iter != state.importStubs.end()) {
			int stubSymId = iter->second;
			return state.symbols.at(stubSymId).RVA(state, image);
		}

		// Check if it's imported from another module
		iter = state.moduleGetGlobals.find(name);
		if (iter != state.moduleGetGlobals.end()) {
			int stubSymId = iter->second;
			return state.symbols.at(stubSymId).RVA(state, image);
		}

		fmt::print(stderr, "Unknown import function: '{}'\n", name);
		hitFatalError = true;
		return 0;
	}

	printf("\tWARN: Unknown storage class %d\n", storageClass);
	return 0;
}

void appendObject(IntermediateState &state, PECOFF &lib) {
	// The list of generated sections which correspond to
	// this object's original sections, with 1:1 matching indices.
	std::vector<GeneratedSection *> generatedInOrder;

	// Again on a 1:1 index mapping, the position each section
	// starts at within it's generated section.
	std::vector<int> sectionOffsets;

	// Copy all the sections from the object into the executable
	for (const Section &sec : lib.sections.list) {
		std::unique_ptr<GeneratedSection> &ptr = state.newSections[sec.name];
		if (!ptr) {
			ptr = std::make_unique<GeneratedSection>();
			ptr->id = state.orderedSections.size();
			ptr->name = sec.name;
			ptr->characteristics = sec.header->Characteristics;
			state.orderedSections.push_back(ptr.get());
		}
		GeneratedSection &out = *ptr;

		generatedInOrder.push_back(ptr.get());

		// If this is an uninitialised section (eg, .bss) then don't actually
		// write any data. The OBJ will set a physical data size, but leave
		// it's physical data pointer at zero. This just indicates how much
		// space is required, it's obviously not asking for the object header
		// to be appended to .bss.
		if (out.IsUninitialised()) {
			sectionOffsets.push_back(out.uninitialisedSize);
			out.uninitialisedSize = alignTo(out.uninitialisedSize + sec.size, 16);
			continue;
		}

		sectionOffsets.push_back(out.data.size());
		out.data.insert(out.data.end(), sec.data, sec.data + sec.size);

		ensureAlignment(out.data, 16);
	}

	// Copy all the symbols into the state.
	int symbolIndexStart = state.symbols.size();
	for (int i = 0; i < lib.fh->NumberOfSymbols; i++) {
		const IMAGE_SYMBOL &sym = lib.symbolTable[i];

		// SectionNumber == 0 means an external symbol, otherwise it's a 1-indexed section id.
		// Less than 0 has some meanings (link below) but it seems to only be used for debugging.
		// https://learn.microsoft.com/en-us/windows/win32/debug/pe-format#section-number-values
		GeneratedSection *section = nullptr;
		int sectionOffset = 0;
		if (sym.SectionNumber > 0) {
			section = generatedInOrder.at(sym.SectionNumber - 1);
			sectionOffset = sectionOffsets.at(sym.SectionNumber - 1);
		}

		const std::string &name = lib.symbolNames.at(i);

		state.symbols.push_back(Symbol{
		    .section = section,
		    .name = name,
		    .originalIndex = i,
		    .value = sectionOffset + sym.Value,
		    .storageClass = sym.StorageClass,
		});

		if (name == "wrenStandaloneMainModule") {
			state.mainModuleGlobals = (int)state.symbols.size() - 1;
		}

		// Other modules will link against this symbol if they import this module.
		// Check if section is null, since if it is then this is an import.
		if (name.ends_with("_get_globals") && section != nullptr) {
			state.moduleGetGlobals[name] = (int)state.symbols.size() - 1;
		}

		// Make the indices match by adding dummy symbols
		i += sym.NumberOfAuxSymbols;
		for (int j = 0; j < sym.NumberOfAuxSymbols; j++) {
			state.symbols.push_back(Symbol{});
		}
	}

	// Copy over all the relocations, now that all the GeneratedSection
	// objects exist and we can look them up by index.
	for (int i = 0; i < lib.sections.list.size(); i++) {
		const Section &sec = lib.sections.list.at(i);
		GeneratedSection &out = *generatedInOrder.at(i);

		for (int j = 0; j < sec.header->NumberOfRelocations; j++) {
			// Manually do the pointer arithmatic, to make sure alignment
			// isn't an issue (the struct is 10 bytes wide, and there isn't
			// any padding in the file).
			IMAGE_RELOCATION reloc = *(IMAGE_RELOCATION *)(lib.fileBase + sec.header->PointerToRelocations + j * 10);

			// Find the current address of the relocation relative to the
			// start of the input section.
			int relocAddr = reloc.VirtualAddress - sec.header->VirtualAddress;

			// Add on the position of the input section within the larger
			// output section we're building.
			relocAddr += sectionOffsets.at(i);

			// Correct the address and add this to the list of this section's
			// required relocations.
			reloc.VirtualAddress = relocAddr;
			out.relocations.push_back(Relocation{
			    .symbolId = symbolIndexStart + (int)reloc.SymbolTableIndex,
			    .offset = relocAddr,
			    .type = reloc.Type,
			});
		}
	}
}

void rewriteExceptions(PECOFF &image, std::vector<ExceptionRecord> &exceptions, GeneratedSection *pdata) {
	if (!image.exceptionsDir)
		return;
	IMAGE_DATA_DIRECTORY *exDir = image.exceptionsDir.value();

	// Combine the original EXE's exception information into the new .pdata2 section.
	// First, read out all the new exception records.
	auto *pdataRec = (IMAGE_AMD64_RUNTIME_FUNCTION_ENTRY *)(image.fileBase + pdata->sectionHeader->PointerToRawData);
	for (const auto *iter = pdataRec; iter->BeginAddress; iter++) {
		if (iter->BeginAddress == 0)
			break;

		exceptions.push_back(ExceptionRecord{
		    .beginRVA = iter->BeginAddress,
		    .endRVA = iter->EndAddress,
		    .unwindRVA = iter->UnwindInfoAddress,
		});
	}

	// Sort the exception records by increasing RVA.
	std::sort(exceptions.begin(), exceptions.end(),
	    [](const ExceptionRecord &a, const ExceptionRecord &b) { return a.beginRVA < b.beginRVA; });

	// Clear out the .pdata section to get rid of any old stuff, then write
	// all our newly-sorted records.
	pdata->data.assign(pdata->data.size(), 0);
	for (int i = 0; i < exceptions.size(); i++) {
		const ExceptionRecord &rec = exceptions.at(i);
		pdataRec[i] = IMAGE_AMD64_RUNTIME_FUNCTION_ENTRY{
		    .BeginAddress = rec.beginRVA,
		    .EndAddress = rec.endRVA,
		    .UnwindInfoAddress = rec.unwindRVA,
		};
	}

	// Add a trailing zero entry for good measure, though it's not documented anywhere.
	// Actually don't, that might overrun the buffer.
	// pdataRec[exceptions.size()] = IMAGE_AMD64_RUNTIME_FUNCTION_ENTRY{0, 0, 0};

	// Now set the exceptions directory to point to our new data.
	exDir->VirtualAddress = pdata->sectionHeader->VirtualAddress;

	// I don't think we need to add one for the trailing zero entry, and it doesn't
	// appear to have that in the original .pdata section.
	exDir->Size = exceptions.size() * sizeof(IMAGE_AMD64_RUNTIME_FUNCTION_ENTRY);
}

int createExportsTable(IntermediateState &state, int &outSize) {
	GeneratedSection *ro = state.newSections.at(".rdata").get();

	// Generate a symbol for the start of the rodata section, as our
	// strings and sections will all reside inside the rodata section.
	int roSym = state.symbols.size();
	state.symbols.push_back(Symbol{
	    .section = ro,
	    .name = "<dll export .rodata>",
	    .originalIndex = -1,
	    .value = 0,
	    .storageClass = IMAGE_SYM_CLASS_STATIC,
	});

	// Write a value that will be relocated into an offset from the
	// start of the section.
	auto writeRVA = [&](int offset) -> int {
		int position = ro->data.size();
		ro->Append(offset);
		ro->relocations.push_back(Relocation{
		    .symbolId = roSym,
		    .offset = position,
		    .type = IMAGE_REL_AMD64_ADDR32NB,
		});
		return position;
	};

	// Update an existing location with a new value.
	// This is for making simple edits without bothering to make
	// a custom symbol for the one thing.
	auto update = [&](int offset, DWORD newValue) { memcpy(ro->data.data() + offset, &newValue, sizeof(newValue)); };

	// Put the exports into a fixed order - we can't trust the
	// map iteration order will remain constant.
	std::vector<std::string> orderedExportNames;
	for (const auto &entry : state.moduleGetGlobals) {
		orderedExportNames.push_back(entry.first);
	}
	int exportCount = orderedExportNames.size();

	// The names have to be sorted to allow binary searches.
	std::sort(orderedExportNames.begin(), orderedExportNames.end());

	// Nothing is more than four bytes in any of the structures.
	ensureAlignment(ro->data, 4);
	int exportDirPosition = ro->data.size();

	// Write out the IMAGE_EXPORT_DIRECTORY
	ro->Append<DWORD>(0);           // Characteristics
	ro->Append<DWORD>(0);           // TimeDateStamp
	ro->Append<WORD>(0);            // MajorVersion
	ro->Append<WORD>(0);            // MinorVersion
	int nameRva = writeRVA(0);      // Name RVA
	ro->Append<DWORD>(1);           // Base - first export is ordinal 1
	ro->Append<DWORD>(exportCount); // NumberOfFunctions
	ro->Append<DWORD>(exportCount); // NumberOfNames

	// The RVA pointers to the tables. Store the addresses
	// of these, and later edit them with the section-relative
	// address. The relocation will turn that into an RVA.
	int addrTableAddr = writeRVA(0);    // AddressOfFunctions
	int nameTableAddr = writeRVA(0);    // AddressOfNames
	int nameOrdTableAddr = writeRVA(0); // AddressOfNameOrdinals

	// Generate the export address table
	update(addrTableAddr, ro->data.size());
	for (const std::string &name : orderedExportNames) {
		int symbolId = state.moduleGetGlobals.at(name);

		int position = ro->data.size();
		ro->Append<DWORD>(0);
		ro->relocations.push_back(Relocation{
		    .symbolId = symbolId,
		    .offset = position,
		    .type = IMAGE_REL_AMD64_ADDR32NB,
		});
	}

	// Generate the export name table
	update(nameTableAddr, ro->data.size());
	std::map<std::string, int> nameRVAOffsets;
	for (const std::string &name : orderedExportNames) {
		nameRVAOffsets[name] = writeRVA(0);
	}

	// Generate the export name-ordinal mappings - this is simple, they're
	// a 1-1 mapping. Note these aren't biased by the Base field in the
	// export directory, so 0 is the first export, not 1 (even though 1 is the
	// ordinal of the first export).
	update(nameOrdTableAddr, ro->data.size());
	for (int i = 0; i < orderedExportNames.size(); i++) {
		ro->Append((uint16_t)i);
	}

	// Generate the export name strings
	for (const auto &entry : state.moduleGetGlobals) {
		update(nameRVAOffsets.at(entry.first), ro->data.size());
		ro->data.insert(ro->data.end(), entry.first.begin(), entry.first.end());
		ro->data.push_back(0); // Null terminator
	}

	// Generate the DLL name
	update(nameRva, ro->data.size());
	std::string dllName = "todo.dll"; // TODO fill this in
	ro->data.insert(ro->data.end(), dllName.begin(), dllName.end());
	ro->data.push_back(0); // Null terminator

	// Generate a symbol to represent the export directory, since that's convenient
	// to get the RVA from later.
	int exportDirSym = state.symbols.size();
	state.symbols.push_back(Symbol{
	    .section = ro,
	    .name = "<dll export table>",
	    .originalIndex = -1,
	    .value = (DWORD)exportDirPosition,
	    .storageClass = IMAGE_SYM_CLASS_STATIC,
	});
	outSize = ro->data.size() - exportDirPosition;
	return exportDirSym;
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

			// We change i later, so keep the original around
			int idx = i;

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

			out.symbolNames.push_back(name);

			// Skip over the auxiliary symbols
			i += sym.NumberOfAuxSymbols;
			for (int j = 0; j < sym.NumberOfAuxSymbols; j++)
				out.symbolNames.push_back("<aux>");

			if (verbose) {
				fmt::print("Symbol {:3} aux={} sc={} type={} section={} value={} '{}'\n", idx, sym.NumberOfAuxSymbols,
				    sym.StorageClass, sym.Type, sym.SectionNumber, (int)sym.Value, name);
			}

			if (sym.StorageClass == IMAGE_SYM_CLASS_FILE) {
				// The following symbol is a filename
				char filenameBuf[sizeof(IMAGE_SYMBOL) + 1];
				ZeroMemory(filenameBuf, sizeof(filenameBuf));
				memcpy(filenameBuf, aux->File.Name, sizeof(IMAGE_SYMBOL));
				if (verbose) {
					fmt::print("  Filename record: {}\n", filenameBuf);
				}
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

		if (verbose)
			fmt::print("Section: {} at {}\n", name, section->VirtualAddress);

		out.sections.AddSection(Section{
		    .id = i,
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
	std::optional<IMAGE_DATA_DIRECTORY> importDir;
	for (int i = 0; i < out.oh->NumberOfRvaAndSizes; i++) {
		IMAGE_DATA_DIRECTORY *dir = &out.oh->DataDirectory[i];
		// fmt::print("Directory {:02d}: {:08x}\n", i, dir->VirtualAddress);

		if (i == 0) {
			exportDir = *dir;
		} else if (i == 1) {
			importDir = *dir;
		} else if (i == 3) {
			out.exceptionsDir = dir;
		}
	}

	if (exportDir && exportDir->VirtualAddress) {
		out.exports.Load(*exportDir, out.sections);
	}
	if (importDir) {
		out.imports.Load(*importDir, out.sections);
	}
}

void createDllImportStubs(IntermediateState &state, const PECOFF &image) {
	// Just assume the text section exists, it'd be pretty silly if it didn't.
	GeneratedSection &sec = *state.newSections.at(".text");
	int textStart = sec.data.size();

	// Our little .text addition
	std::vector<uint8_t> text;

	for (const auto &[name, rva] : image.imports.importNameRVAs) {
		// We only care about the Wren generated-entry functions, and any
		// modules bundled with the runtime.
		if (!name.starts_with("wren_") && !name.ends_with("_get_globals"))
			continue;

		// Load the address of the function (which we get from the
		// values populated by Windows) into RAX (which is always
		// safe to overwrite), and jump to it.

		// mov rax, [rsp+0]
		int funcPosition = textStart + text.size();
		text.push_back(0x48);
		text.push_back(0x8b);
		text.push_back(0x05);

		int addressPosition = textStart + text.size();
		text.push_back(0);
		text.push_back(0);
		text.push_back(0);
		text.push_back(0);

		// jmp rax
		text.push_back(0xff);
		text.push_back(0xe0);

		// Add a symbol for the objects to use
		state.importStubs[name] = state.symbols.size();
		state.symbols.push_back(Symbol{
		    .section = &sec,
		    .name = name,
		    .value = (DWORD)funcPosition,
		    .storageClass = IMAGE_SYM_CLASS_STATIC,
		});

		// Add a symbol/relocation pair to set the relative offset
		// in the mov instruction.
		sec.relocations.push_back(Relocation{
		    .symbolId = (int)state.symbols.size(),
		    .offset = addressPosition,
		    .type = IMAGE_REL_AMD64_REL32,
		});
		state.symbols.push_back(Symbol{
		    .name = name,
		    .isImportReloc = true,
		});
	}

	sec.data.insert(sec.data.end(), text.begin(), text.end());
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

		if (verbose)
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
