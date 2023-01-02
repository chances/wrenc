//
// Created by znix on 11/12/22.
//

#pragma once

#include "Module.h"

struct CompilationOptions {
	bool includeDebugInfo = false;
	bool forceAssemblyOutput = false;
	bool enableGCSupport = false;
};

struct CompilationResult {
	enum Format {
		OBJECT,
		ASSEMBLY,
		QBE_IR,
	};

	bool successful = false;
	std::vector<uint8_t> data;
	int fd = -1;
	std::string tempFilename;
	Format format = OBJECT;
};

class IBackend {
  public:
	virtual ~IBackend();

	virtual CompilationResult Generate(Module *mod, const CompilationOptions *options) = 0;

	/// Very special case flag for compiling Wren 'standard library' stuff, like the methods for List and String.
	/// This makes things behave a bit... weird, disabling some parts of the class generation so we can cleanly
	/// add the methods onto core types defined in C++.
	bool compileWrenCore = false;

	/// If true, this generates the wrenStandaloneMainModule symbol. This should only be enabled for a single module.
	bool defineStandaloneMainModule = false;
};
