//
// Created by znix on 24/12/22.
//

#pragma once

#include <llvm/CodeGen/GCMetadataPrinter.h>
#include <llvm/IR/GCStrategy.h>

class WrenGCMetadataPrinter : public llvm::GCMetadataPrinter {
  public:
	~WrenGCMetadataPrinter();

	bool emitStackMaps(llvm::StackMaps &maps, llvm::AsmPrinter &printer) override;

	static const char *GetStackMapSymbol();
};
