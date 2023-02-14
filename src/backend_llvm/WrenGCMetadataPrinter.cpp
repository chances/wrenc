//
// Created by znix on 24/12/22.
//

#include "WrenGCMetadataPrinter.h"
#include "WrenGCStrategy.h"
#include "common/StackMapDescription.h"
#include "common/common.h"

#include <fmt/format.h>
#include <llvm/CodeGen/AsmPrinter.h>
#include <llvm/MC/MCContext.h>
#include <llvm/MC/MCObjectFileInfo.h>
#include <llvm/MC/MCRegisterInfo.h>
#include <llvm/MC/MCStreamer.h>
#include <llvm/Target/TargetMachine.h>

#include <deque>

// There's a lot of nested structs we need to access, so use a short alias
using SM = llvm::StackMaps;
using SMD = StackMapDescription;
using ObjectID = SMD::ObjectID;

// The function info itself doesn't contain a pointer to the start of the function
using FullFuncInfo = std::pair<const llvm::MCSymbol *, const SM::FunctionInfo *>;

// https://refspecs.linuxbase.org/elf/x86_64-abi-0.99.pdf - section 3.36
constexpr int RSP_DWARF_ID = 7;

// Note our name must match the GC strategy, as that's how this printer is selected from the registry
static const llvm::GCMetadataPrinterRegistry::Add<WrenGCMetadataPrinter> REG(WrenGCStrategy::GetStrategyName(),
    "A metadata printer compatible with the qwrencc runtime");

const char *WrenGCMetadataPrinter::GetStackMapSymbol() { return "stackmaps"; }

WrenGCMetadataPrinter::~WrenGCMetadataPrinter() = default;

static llvm::Align eightByteAlignment(8);

static void writeObjectHeader(llvm::AsmPrinter &printer, const SMD::MapObjectRepr &obj) {
	llvm::MCStreamer &os = *printer.OutStreamer;
	os.emitInt16((int)obj.id);
	os.emitInt16(obj.sectionSize);
	os.emitInt16(obj.flags);
	os.emitInt16(obj.forObject);
}

static int64_t checkConstLoc(const SM::LocationVec &vec, size_t idx, const char *name) {
	if (vec.size() <= idx) {
		fmt::print(stderr, "Stackmap entry location vector truncated for {}\n", name);
		abort();
	}

	const SM::Location &loc = vec[idx];
	if (loc.Type != SM::Location::Constant) {
		fmt::print(stderr, "Stackmap entry location vector: wrong type {} for {}\n", (int)loc.Type, name);
		abort();
	}

	return loc.Offset;
}

static int alignTo64(int value) {
	int overhang = value % 8;
	if (overhang == 0)
		return value;
	value += 8 - overhang;
	return value;
}

static void writeFunction(FullFuncInfo func, llvm::AsmPrinter &printer) {
	llvm::MCStreamer &os = *printer.OutStreamer;

	SMD::MapObjectRepr repr = {
	    .id = ObjectID::FUNCTION,
	    .sectionSize = 16,
	};
	writeObjectHeader(printer, repr);
	os.emitSymbolValue(func.first, 8); // Function pointer
	os.emitInt32(func.second->RecordCount);
	os.emitInt32(func.second->StackSize);

	// Write the function's name
	llvm::StringRef name = func.first->getName();
	SMD::MapObjectRepr nameRepr = {
	    .id = ObjectID::OBJECT_NAME,
	    .sectionSize = (uint16_t)alignTo64(name.size()),
	    .forObject = (uint16_t)name.size(),
	};
	writeObjectHeader(printer, nameRepr);
	os.emitBytes(name);
	os.emitValueToAlignment(eightByteAlignment);
}

bool WrenGCMetadataPrinter::emitStackMaps(llvm::StackMaps &maps, llvm::AsmPrinter &printer) {
	llvm::MCContext &outContext = printer.OutStreamer->getContext();
	llvm::MCStreamer &os = *printer.OutStreamer;

	// Use the data section on platforms that don't support relro (Windows/OSX)
	llvm::MCSection *stackMapSection = outContext.getObjectFileInfo()->getDataRelROSection();
	if (!stackMapSection)
		stackMapSection = outContext.getObjectFileInfo()->getDataSection();
	os.switchSection(stackMapSection);

	// Start aligned to the eight-byte boundary, so we can safely read 64-bit values on all architectures
	os.emitValueToAlignment(eightByteAlignment);

	// This is the label the generated Wren code links to
	os.emitLabel(outContext.getOrCreateSymbol(GetStackMapSymbol()));
	os.emitInt16(1); // Major version (breaking changes)
	os.emitInt16(0); // Minor version (compatible changes)
	os.emitInt16(0); // Flags
	os.emitInt16(0); // Unused

	// Put the functions into a deque. Each function has a count for the number of callsites it contains, and
	// the instruction pointer in those callsites are all relative to the start of the function. We'll thus
	// write a function entry before all the callsites that it contains.
	// Thus the actual writing of these functions will be done in the callsites loop.
	std::deque<FullFuncInfo> functions;
	for (const auto &entry : maps.getFnInfos()) {
		functions.push_back({entry.first, &entry.second});
	}

	// Track how many callsites are left before we need to progress to the next function
	int remainingCallsites = -1;

	for (const SM::CallsiteInfo &cs : maps.getCSInfos()) {
		// Write out functions until we find the next one that has at least one unwritten callsite.
		while (remainingCallsites < 1) {
			writeFunction(functions.front(), printer);
			remainingCallsites = functions.front().second->RecordCount;
			functions.pop_front();
		}
		remainingCallsites--;

		// AFAIK liveouts are only for statepoints/patchpoints, and should never occur for statepoints.
		if (!cs.LiveOuts.empty()) {
			fmt::print(stderr, "Found non-empty liveout array in stackmap generation!\n");
			abort();
		}

		// The first three entries in the locations table are constants for specific properties
		// https://llvm.org/docs/Statepoints.html#stack-map-format
		int pos = 0;
		int64_t callingConv = checkConstLoc(cs.Locations, pos++, "calling convention");
		int64_t statepointFlags = checkConstLoc(cs.Locations, pos++, "statepoint flags");
		int64_t numDeoptLocs = checkConstLoc(cs.Locations, pos++, "deopt count");
		(void)callingConv;
		(void)statepointFlags;

		if (numDeoptLocs != 0) {
			fmt::print(stderr, "Found non-empty deopt location count in stackmap generation!\n");
			abort();
		}

		std::vector<int> valueOffsets;

		while (pos < (int)cs.Locations.size()) {
			int remaining = cs.Locations.size() - pos;
			if (remaining < 2) {
				fmt::print(stderr, "Found odd number of remaining stackmap locations: {} (from {})\n", remaining,
				    (int)cs.Locations.size());
				abort();
			}

			// There are two entries - this is for values that are a pointer inside an object, for example to read
			// an int from an object. We can't read into an object other than 'this', so they should always be the same.
			// However, the pass that generates these isn't always perfect and can end up splitting a single variable
			// into base and derived values, which both have the same values.
			const SM::Location &base = cs.Locations[pos++];
			const SM::Location &derived = cs.Locations[pos++];
			(void)derived;

			// If we've found a constant, that'll be for a floating-point literal that RS4GC found, and can safely
			// be ignored (after all, we don't have the address of anything dynamically allocated at compile time).
			if (base.Type == SM::Location::Constant || base.Type == SM::Location::ConstantIndex) {
				continue;
			}

			// Everything should be spilled onto the stack, so it can be relocated.
			if (base.Type != SM::Location::Indirect) {
				fmt::print(stderr, "Found non-indirect stackmap entry\n");
				abort();
			}

			if (base.Size != sizeof(Value)) {
				fmt::print(stderr, "Found wrong-sized stackmap location: size={}\n", (int)base.Size);
				abort();
			}

			// Only support RSP-relative addresses, as that's all LLVM appears to use (might have to add support
			// for RBP-relative addresses in the future?)
			if (base.Reg != RSP_DWARF_ID) {
				fmt::print(stderr, "Found non-RSP-relative stackmap entry. DWARF Register ID: {}\n", base.Reg);
				abort();
			}

			// Nothing should be below the stack pointer!
			if (base.Offset < 0) {
				fmt::print(stderr, "Found negative RSP-relative stackmap entry! Offset {}\n", base.Offset);
				abort();
			}

			// Require that the offset is 8-byte-aligned (which it should be - we're only using 64-bit values).
			// This requirement means we can divide all the offsets by 8, letting us use only a single byte to
			// support functions with upto 2048 variables.
			if (base.Offset % 8 != 0) {
				fmt::print(stderr, "Found non-64-bit aligned stackmap entry! Offset {}\n", base.Offset);
				abort();
			}

			uint8_t packedOffset = (uint8_t)(base.Offset / 8);

			if (base.Offset != packedOffset * 8) {
				fmt::print(stderr, "Found unpackable stackmap entry offset {}\n", base.Offset);
				abort();
			}

			valueOffsets.push_back(packedOffset);
		}

		if (valueOffsets.size() >= UINT16_MAX) {
			fmt::print(stderr, "Found too many value offsets for stackmap entry: {}\n", valueOffsets.size());
			abort();
		}

		// 4 for the offset to the call, then one byte per offset.
		// We then round up to a multiple of 8, so it's properly aligned.
		int size = alignTo64(4 + valueOffsets.size());

		if (size >= UINT16_MAX) {
			fmt::print(stderr, "Stackmap statepoint entry too large: {}\n", valueOffsets.size());
			abort();
		}

		SMD::MapObjectRepr repr = {
		    .id = ObjectID::STATEPOINT,
		    .sectionSize = (uint16_t)size,
		    .forObject = (uint16_t)valueOffsets.size(),
		};
		writeObjectHeader(printer, repr);

		// This is the pointer to the statepoint minus the function pointer, so four bytes is plenty
		os.emitValue(cs.CSOffsetExpr, 4);

		for (uint8_t offset : valueOffsets) {
			os.emitInt8(offset);
		}

		os.emitValueToAlignment(eightByteAlignment);
	}

	// Write out any remaining functions that don't have statepoints
	for (FullFuncInfo func : functions) {
		writeFunction(func, printer);
	}

	// Write a special section to mark the end of the stackmap
	writeObjectHeader(printer, {.id = ObjectID::END_OF_STACK_MAP});

	return true;
}
