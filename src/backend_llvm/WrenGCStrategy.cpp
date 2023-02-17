//
// Created by znix on 23/12/22.
//

#include "WrenGCStrategy.h"

#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Type.h>

WrenGCStrategy::WrenGCStrategy() {
	UseStatepoints = true;

	// A patch landed in what will be LLVM 17 (D141110) requires this flag
	// whenever RS4GC is to be used. Prior LLVM versions had a hardcoded
	// list of strategy names that needed to be patched.
	UseRS4GC = true;

	// These options are all gc.root specific, we specify them so that the
	// gc.root lowering code doesn't run.
	NeededSafePoints = false;

	// This needs to be set to use our custom statepoint writing thing in WrenGCMetadataPrinter
	UsesMetadata = true;
}

std::optional<bool> WrenGCStrategy::isGCManagedPointer(const llvm::Type *type) const {
	// Method is only valid on pointer typed values.
	const llvm::PointerType *pointer = llvm::cast<llvm::PointerType>(type);
	// For the sake of this example GC, we arbitrarily pick addrspace(1) as our
	// GC managed heap.  We know that a pointer into this heap needs to be
	// updated and that no other pointer does.  Note that addrspace(1) is used
	// only as an example, it has no special meaning, and is not reserved for
	// GC usage.
	return (VALUE_ADDR_SPACE == pointer->getAddressSpace());
}

const char *WrenGCStrategy::GetStrategyName() { return "wrengc"; }

static llvm::GCRegistry::Add<WrenGCStrategy> gcReg(WrenGCStrategy::GetStrategyName(), "wren's custom GC strategy");
