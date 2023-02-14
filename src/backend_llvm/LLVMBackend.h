//
// Created by znix on 09/12/22.
//

#pragma once

#include "ArenaAllocator.h"
#include "IBackend.h"
#include "Module.h"
#include "wrencc_config.h"

#ifdef USE_LLVM

// This class is subclassed in LLVMBackend.cpp, to avoid importing LLVM here
class LLVMBackend : public IBackend {
  public:
	// Get an instance of the file-local implementation
	static std::unique_ptr<LLVMBackend> Create();

	virtual ~LLVMBackend();

  private:
	ArenaAllocator m_alloc;
};

// Non-name-mangled function for dynamic loading
extern "C" {
DLL_EXPORT LLVMBackend *create_llvm_backend(); // NOLINT(readability-identifier-naming)
}

#endif
