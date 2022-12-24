//
// Created by znix on 23/12/22.
//

#pragma once

#include <llvm/IR/GCStrategy.h>

class WrenGCStrategy : public llvm::GCStrategy {
  public:
	WrenGCStrategy();

	std::optional<bool> isGCManagedPointer(const llvm::Type *type) const override;

	/// We pretend that a Value is a pointer in another address space. This helps with the safepoint stuff.
	static constexpr int VALUE_ADDR_SPACE = 1;

	/// Get the name of this strategy, as should be set on functions. This is a non-inline function to force
	/// the linker to include the GC registration if this is used.
	static const char *GetStrategyName();
};
