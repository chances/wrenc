#include <fmt/format.h>

#include "IRNode.h"
#include "IRPrinter.h"
#include "RunProgramme.h"
#include "Utils.h"
#include "backend_qbe/QbeBackend.h"
#include "common/Platform.h"
#include "passes/BasicBlockPass.h"
#include "passes/IRCleanup.h"
#include "wren_compiler.h"
#include "wrencc_config.h"

#include <fcntl.h>
#include <fstream>
#include <signal.h>
#include <sstream>

#if _WIN32
#include <ya_getopt.h>
#else
#include <dlfcn.h>
#include <getopt.h>
#endif

static const std::string QBE_PATH = "lib/qbe-1.0/qbe_bin";

enum class OutputType {
	OT_EXEC,
	OT_LIB_STATIC,
	OT_LIB_SHARED,
};

std::string filenameForFd(int fd);
static std::string runQbe(std::string qbeIr);
static CompilationResult runCompiler(const std::istream &input, const std::string &moduleName,
    const std::optional<std::string> &sourceFileName, bool main, const CompilationOptions *opts);
static void runAssembler(const std::vector<std::string> &assemblyFiles, const std::string &outputFilename);
static void runLinker(const std::string &outputPath, const std::vector<std::string> &objectFiles, OutputType type);
static void staticLink(const std::string &outputFile, const std::vector<std::string> &objectFiles);
static int printSysPath(const std::string &name);

static std::string compilerInstallDir;

enum SelectedBackend : int {
	BACKEND_AUTO,
	BACKEND_QBE,
	BACKEND_LLVM,
};

static int globalDontAssemble = 0;
static int globalBuildCoreLib = 0;
static int globalNoDebugInfo = 0;
static int globalDisableGC = 0;
static int globalVerbose = 0;
static SelectedBackend globalSelectedBackend = BACKEND_AUTO;

typedef IBackend *(*createBackendFunc_t)();
static createBackendFunc_t createLLVMBackend = nullptr;

enum LongOnlyOpts {
	PRINT_SYS_PATH = 1,
};

static option options[] = {
    {"output", required_argument, 0, 'o'},
    {"compile-only", no_argument, 0, 'c'},
    {"module", required_argument, 0, 'm'},
    {"optimise", required_argument, 0, 'O'},
    {"dont-assemble", no_argument, &globalDontAssemble, true},
    {"help", no_argument, 0, 'h'},
    {"no-debug-info", no_argument, &globalNoDebugInfo, true},
    {"disable-gc", no_argument, &globalDisableGC, true},
    {"verbose", no_argument, &globalVerbose, true},
    {"output-type", required_argument, 0, 't'},
    {"print-sys-path", required_argument, 0, PRINT_SYS_PATH},

    // Don't put the LLVM backend behind an ifdef flag, if the user issues it
    // we'll give them a more descriptive warning about LLVM being disabled.
    {"backend-qbe", no_argument, (int *)&globalSelectedBackend, BACKEND_QBE},
    {"backend-llvm", no_argument, (int *)&globalSelectedBackend, BACKEND_LLVM},

    // Intentionally undocumented options
    {"internal-build-core-lib", no_argument, &globalBuildCoreLib, true}, // Build the wren_core module

    {0},
};

int main(int argc, char **argv) {
#ifndef _WIN32
	// Ignore SIGPIPE that might be caused if QBE or the assembler (run as child processes) crash - we can handle
	// the error and generate a proper error message, so turn this off.
	signal(SIGPIPE, SIG_IGN);
#endif

	// Find the directory of our executable, as we'll need libraries from the same directory later.
	{
		// Chop off the executable name, leaving just the path
		compilerInstallDir = plat_util::getExeName();
		size_t strokePos = compilerInstallDir.find_last_of(plat_util::PATH_SEPARATOR);
		compilerInstallDir.erase(strokePos);
	}

	bool needsHelp = false; // Print the help page?
	bool optError = false;
	bool compileOnly = false;
	std::string outputFile;
	std::vector<std::string> moduleNames;

	CompilationOptions backendOpts;
	OutputType outputType = OutputType::OT_EXEC;

	while (true) {
		int opt = getopt_long(argc, argv, "o:m:O:t:ch", options, nullptr);

		// -1 means we ran out of options
		if (opt == -1)
			break;

		switch (opt) {
		case 'o':
			outputFile = optarg;
			break;
		case 'c':
			compileOnly = true;
			break;
		case 'h':
			needsHelp = true;
			break;
		case 'm':
			moduleNames.push_back(optarg);
			break;
		case 'g':
			backendOpts.includeDebugInfo = true;
			break;
		case 'O': {
			std::string arg = optarg;
			if (arg == "none") {
				backendOpts.optimisationLevel = WrenOptimisationLevel::NONE;
			} else if (arg == "fast") {
				backendOpts.optimisationLevel = WrenOptimisationLevel::FAST;
			} else {
				fmt::print(stderr, "Invalid optimisation level '{}', see --help\n", argv[0]);
				optError = true;
			}
			break;
		}
		case 't': {
			std::string arg = optarg;
			if (arg == "exec") {
				outputType = OutputType::OT_EXEC;
			} else if (arg == "static") {
				outputType = OutputType::OT_LIB_STATIC;
			} else if (arg == "shared") {
				outputType = OutputType::OT_LIB_SHARED;
			} else {
				fmt::print(stderr, "Invalid output type '{}', see --help\n", arg);
				optError = true;
			}
			break;
		}
		case PRINT_SYS_PATH: {
			// As a special case, just execute this immediately, since it's
			// not going to be part of a complex command line.
			return printSysPath(optarg);
		}
		case '?':
			// Error message already printed by getopt
			fmt::print(stderr, "For a list of options, run {} --help\n", argv[0]);
			exit(1);
		case 0:
			// This option set a flag specified in the options table, we don't have to do anything
			break;
		default:
			// This shouldn't happen, invalid options come back as '?'
			fmt::print(stderr, "Unhandled option value '{}'\n", opt);
			abort();
		}
	}

	backendOpts.includeDebugInfo = !globalNoDebugInfo;
	backendOpts.forceAssemblyOutput = globalDontAssemble;
	backendOpts.enableGCSupport = !globalDisableGC;
	RunProgramme::printCommands = globalVerbose;

	if (needsHelp) {
		fmt::print("Usage: {} [-hc] [-o «filename»] inputs...\n", argv[0]);

		std::vector<std::tuple<std::string, std::string, std::string>> optHelp;
		optHelp.emplace_back("-h", "--help", "Print this help page");
		optHelp.emplace_back("-c", "--compile-only", "Only compile the input files, do not link them");
		optHelp.emplace_back("-o file", "--output=file", "Write the output to the file [file]");
		optHelp.emplace_back("-m name", "--module=name", "Sets the module name. Repeat for multiple files.");
		optHelp.emplace_back("-O level", "--optimise=level", "Sets the optimisation mode, one of 'none' or 'fast'");
		optHelp.emplace_back("-t type", "--output-type=type",
		    "Sets the type of file to make, one of exec, static or shared.");

		optHelp.emplace_back("", "--dont-assemble", "Write out an assembly file");
		optHelp.emplace_back("", "--no-debug-info", "Don't include any debugging information");
		optHelp.emplace_back("", "--disable-gc", "Don't include any GC-support code in the generated assembly");
		optHelp.emplace_back("", "--verbose", "Print diagnostic information, eg linker command line");

		optHelp.emplace_back("", "--backend-qbe", "Use the QBE backend");
#ifdef USE_LLVM
		optHelp.emplace_back("", "--backend-llvm", "Use the LLVM backend");
#endif

		optHelp.emplace_back("", "--print-sys-path=name",
		    "Print the name of an important wrenc component. Try 'help' for a list.");

		optHelp.emplace_back("", "inputs...", "The Wren source files to compile, or object files to link");

		// Find the longest argument string, so we can line everything up
		int maxLengths[2] = {0, 0};
		for (const auto &tuple : optHelp) {
			std::string shortOpt, longOpt, help;
			std::tie(shortOpt, longOpt, help) = tuple;
			maxLengths[0] = std::max(maxLengths[0], (int)shortOpt.size());
			maxLengths[1] = std::max(maxLengths[1], (int)longOpt.size());
		}

		for (const auto &tuple : optHelp) {
			std::string shortOpt, longOpt, help;
			std::tie(shortOpt, longOpt, help) = tuple;

			while ((int)shortOpt.size() < maxLengths[0])
				shortOpt.push_back(' ');
			while ((int)longOpt.size() < maxLengths[1])
				longOpt.push_back(' ');

			fmt::print(" {} {}  {}\n", shortOpt, longOpt, help);
		}

		exit(0);
	}

	// Check for option errors after printing the help page, if requested.
	// This is so you can toss -h onto a malformed command.
	if (optError)
		return 1;

	if (outputFile.empty()) {
		fmt::print(stderr, "{}: missing option --output. Use --help for more information.\n", argv[0]);
		exit(1);
	}

	// A '-' means read/write to stdin/stdout
	if (outputFile == "-") {
		outputFile = "/dev/stdout";
	}

	// The remaining arguments are input filenames - either source or object files
	std::vector<std::string> sourceFiles;
	std::vector<std::string> objectFiles;
	for (int i = optind; i < argc; i++) {
		std::string file = argv[i];
		if (file.ends_with(".o") || file.ends_with(".obj")) {
			objectFiles.push_back(file);
		} else if (file == "-") {
			sourceFiles.push_back("/dev/stdin");
		} else {
			sourceFiles.push_back(file);
		}
	}

	if (sourceFiles.empty() && objectFiles.empty()) {
		fmt::print(stderr, "{}: No source files specified. Use --help for more information.\n", argv[0]);
		exit(1);
	}

	if (moduleNames.size() > sourceFiles.size()) {
		fmt::print(stderr, "{}: More module names specified than source files.\n", argv[0]);
		exit(1);
	}

	if (globalSelectedBackend == BACKEND_AUTO) {
		// Default to LLVM
#ifdef USE_LLVM
		globalSelectedBackend = BACKEND_LLVM;
#else
		globalSelectedBackend = BACKEND_QBE;
#endif

		// Let the user select QBE or LLVM explicitly
		const char *envUseLLVM = getenv("USE_LLVM");
		if (envUseLLVM && envUseLLVM == std::string("1")) {
			globalSelectedBackend = BACKEND_LLVM;
		} else if (envUseLLVM) {
			// Something other than '1' was set
			globalSelectedBackend = BACKEND_QBE;
		}
	}

	std::vector<std::string> assemblyFiles;
	bool hitError = false;
	for (size_t sourceFileId = 0; sourceFileId < sourceFiles.size(); sourceFileId++) {
		const std::string &sourceFile = sourceFiles.at(sourceFileId);
		std::string thisModuleName;
		if (sourceFileId < moduleNames.size()) {
			thisModuleName = moduleNames.at(sourceFileId);
		} else {
			thisModuleName = "unnamed_module_" + std::to_string(sourceFileId);
		}
		std::ifstream input;
		try {
			input.exceptions(std::ios::badbit | std::ios::failbit);
			input.open(sourceFile);
			// TODO add a proper way of selecting the main module
			bool isMain = sourceFileId == 0 && outputType == OutputType::OT_EXEC;
			CompilationResult result = runCompiler(input, thisModuleName, sourceFile, isMain, &backendOpts);
			if (!result.successful) {
				hitError = true;
				continue;
			}
			switch (result.format) {
			case CompilationResult::OBJECT:
				if (result.tempFilename.empty()) {
					fmt::print(stderr, "Cannot process non-tempfile assembly compilation result for {}\n", sourceFile);
					exit(1);
				}
				objectFiles.push_back(result.tempFilename);
				break;
			case CompilationResult::ASSEMBLY:
				if (!result.tempFilename.empty()) {
					assemblyFiles.push_back(result.tempFilename);
				} else {
					fmt::print(stderr, "Cannot process non-filename assembly compilation result for {}\n", sourceFile);
					exit(1);
				}
				break;
			default:
				fmt::print(stderr, "Unsupported compilation result format {} for {}\n", result.format, sourceFile);
				exit(1);
			}
		} catch (const std::fstream::failure &ex) {
			fmt::print(stderr, "Failed to read source file {}: {}\n", sourceFile, ex.what());
			exit(1);
		}
	}

	// Wait until now to print the error, to get errors from multiple files if we're compiling multiple files at once.
	if (hitError) {
		fmt::print(stderr, "Parse errors found, aborting\n");
		return 1;
	}

	if (globalDontAssemble) {
		// Join all the assembly files together
		std::ofstream output;
		output.exceptions(std::ios::badbit | std::ios::failbit);
		try {
			output.open(outputFile);
			for (const std::string &assemblyFile : assemblyFiles) {
				std::ifstream input;
				input.exceptions(std::ios::badbit | std::ios::failbit);
				input.open(assemblyFile);
				output << input.rdbuf();
			}
		} catch (const std::fstream::failure &ex) {
			fmt::print(stderr, "Failed to write assembly: {}\n", ex.what());
			exit(1);
		}
	} else if (compileOnly) {
		if (objectFiles.size() == 1) {
			// Copy over the object into the output file
			std::ifstream input;
			std::ofstream output;
			try {
				input.exceptions(std::ios::badbit | std::ios::failbit);
				output.exceptions(std::ios::badbit | std::ios::failbit);

				input.open(objectFiles.at(0), std::ios::binary);
				output.open(outputFile, std::ios::binary);

				// Copy the whole thing across
				output << input.rdbuf();
			} catch (const std::fstream::failure &ex) {
				fmt::print(stderr, "Failed to copy object file: {}\n", ex.what());
				exit(1);
			}
		} else if (!objectFiles.empty()) {
			// We could link together the two or more object files, but really
			// the user can just compile them separately.
			fmt::print(stderr, "Cannot use -c option with more than one output object file\n");
			exit(1);
		} else {
			// We must be using assembly files
			runAssembler(assemblyFiles, outputFile);
		}
	} else {
		// FIXME use a proper temporary file
		if (!assemblyFiles.empty()) {
			runAssembler(assemblyFiles, "/tmp/wren-output-test.o");
			objectFiles.push_back("/tmp/wren-output-test.o");
		}
		runLinker(outputFile, objectFiles, outputType);
	}
}

int printSysPath(const std::string &name) {
	// This sub-command prints out the paths to various components, like
	// the headers directory or runtime library. It's principally intended
	// to be used in buildscripts, to discover this information from just
	// the compiler.

	if (name == "help") {
		fmt::print("List of available paths:\n");
		fmt::print("\theader-dir\n");
		fmt::print("\trtlib-dir\n");
		fmt::print("\trtlib-name\n");
		return 0;
	}

	// TODO use the correct paths when installed

	if (name == "header-dir") {
		fmt::print("{}/../pub_include\n", compilerInstallDir);
		return 0;
	}
	if (name == "rtlib-dir") {
		fmt::print("{}\n", compilerInstallDir);
		return 0;
	}
	if (name == "rtlib-name") {
		fmt::print("libwren-rtlib.so\n");
		return 0;
	}

	fmt::print(stderr, "Invalid system-path component name '{}'\n", name);
	return 1;
}

static CompilationResult runCompiler(const std::istream &input, const std::string &moduleName,
    const std::optional<std::string> &sourceFileName, bool main, const CompilationOptions *opts) {

	std::string source;
	{
		// Quick way to read an entire stream
		std::stringstream buf;
		buf << input.rdbuf();
		source = buf.str();
	}

	CompContext ctx;
	Module mod(moduleName);

	// Use the absolute path to the source file, so debugging works wherever it's run (on the same system, at least).
	if (sourceFileName) {
		mod.sourceFilePath = plat_util::resolveFilename(sourceFileName.value());
	}

	IRFn *rootFn = wrenCompile(&ctx, &mod, source.c_str(), false, globalBuildCoreLib);

	if (!rootFn)
		return CompilationResult{.successful = false};

	IRCleanup cleanup(&ctx.alloc);
	for (IRFn *fn : mod.GetFunctions())
		cleanup.Process(fn);

	BasicBlockPass ssa(&ctx.alloc);
	for (IRFn *fn : mod.GetFunctions())
		ssa.Process(fn);

	IRPrinter printer;
	for (IRFn *fn : mod.GetFunctions())
		printer.Process(fn);
	std::unique_ptr<std::stringstream> dbg = printer.Extract();
	// fmt::print("AST/IR:\n{}\n", dbg->str());
	std::ofstream irOutput;
	irOutput.exceptions(std::ios::badbit | std::ios::failbit);
	std::string wrencIrFile = "/tmp/wren_wrenc_ir.txt";
	try {
		irOutput.open(wrencIrFile);
		irOutput << dbg->str() << std::endl;
	} catch (const std::fstream::failure &ex) {
		fmt::print(stderr, "Failed to write wren IR: {}\n", ex.what());
		exit(1);
	}

	std::unique_ptr<IBackend> backend;
	if (globalSelectedBackend == BACKEND_LLVM) {
#ifdef USE_LLVM
		// Dynamically load the LLVM backend - this is so the frontend can be easily debugged
		// without waiting for GDB to load all of LLVM's symbols.
		if (!createLLVMBackend) {
			std::string llvmBackendFilename = compilerInstallDir + "/llvm_backend.so";
			void *handle = dlopen(llvmBackendFilename.c_str(), RTLD_NOW);
			if (!handle) {
				fmt::print(stderr, "Failed to load LLVM backend: {}\n", dlerror());
				exit(1);
			}
			typedef IBackend *(*createFunc_t)();
			createLLVMBackend = (createFunc_t)dlsym(handle, "create_llvm_backend");
			if (!createLLVMBackend) {
				fmt::print(stderr, "Could not find backend creation function in the LLVM backend module\n");
				exit(1);
			}
		}
		backend = std::unique_ptr<IBackend>(createLLVMBackend());
#else
		fmt::print(stderr, "LLVM support is not included in this build, ensure Meson's use_llvm is enabled\n");
		exit(1);
#endif
	} else if (globalSelectedBackend == BACKEND_QBE) {
		backend = std::unique_ptr<IBackend>(new QbeBackend());
	} else {
		fmt::print(stderr, "Invalid selected backend {}\n", globalSelectedBackend);
		abort();
	}

	backend->compileWrenCore = globalBuildCoreLib;
	backend->defineStandaloneMainModule = !globalBuildCoreLib && main;
	CompilationResult result = backend->Generate(&mod, opts);

	if (!result.successful) {
		fmt::print(stderr, "Failed to compile module, backend was unsuccessful.\n");
		exit(1);
	}

	if (result.format != CompilationResult::QBE_IR)
		return result;

	if (result.data.empty()) {
		fmt::print(stderr, "Cannot compile non-array QBE IR\n");
		exit(1);
	}

	std::string qbeIr(result.data.begin(), result.data.end());
	// fmt::print("Generated QBE IR:\n{}\n", qbeIr);

	std::ofstream output;
	output.exceptions(std::ios::badbit | std::ios::failbit);
	std::string irFile = "/tmp/wren_qbe_ir.qbe";
	try {
		output.open(irFile);
		output << qbeIr << std::endl;
	} catch (const std::fstream::failure &ex) {
		fmt::print(stderr, "Failed to write QBE IR: {}\n", ex.what());
		exit(1);
	}

	std::string tempFilename = runQbe(qbeIr);
	return CompilationResult{
	    .successful = true,
	    .tempFilename = tempFilename,
	    .format = CompilationResult::ASSEMBLY,
	};
}

std::string filenameForFd(int fd) { return fmt::format("/dev/fd/{}", fd); }

static std::string runQbe(std::string qbeIr) {
	// Create a temporary file to store the output
	// This creates a file that's deleted when the programme exists.
	std::string outputFilename = utils::buildTempFilename("qbe-output-!!!!!!!!.S");

	// Find the path to QBE, relative to this executable
	std::string qbePath = compilerInstallDir + "/" + QBE_PATH;

	// Run the programme
	RunProgramme prog;
	prog.args.push_back(qbePath);
	prog.args.push_back("-o");
	prog.args.push_back(outputFilename);
	prog.input = std::move(qbeIr);
	prog.Run();

	return outputFilename;
}

static void runAssembler(const std::vector<std::string> &assemblyFiles, const std::string &outputFilename) {
	RunProgramme prog;
	prog.args.push_back("as");
	prog.args.push_back("-o");
	prog.args.push_back(outputFilename);
	for (const std::string &filename : assemblyFiles) {
		prog.args.push_back(filename);
	}
	prog.withEnv = true;
	prog.Run();
}

static void runLinker(const std::string &outputPath, const std::vector<std::string> &objectFiles, OutputType type) {
	// Static linking doesn't use the linker, since it's just a bunch of object files
	if (type == OutputType::OT_LIB_STATIC) {
		return staticLink(outputPath, objectFiles);
	}

	RunProgramme prog;
	prog.args.push_back("ld.gold"); // Using lld would be even better, but it can't find -lc by itself
	prog.args.push_back("-o");
	prog.args.push_back(outputPath);

	// Link it to the runtime library
	prog.args.push_back(compilerInstallDir + "/libwren-rtlib.so");

	// Tell the dynamic link loader where to find the runtime library
	// TODO handle rtlib in some non-hardcoding-paths way on release builds
	prog.args.push_back("-rpath=" + compilerInstallDir);

	prog.args.insert(prog.args.end(), objectFiles.begin(), objectFiles.end());
	prog.args.push_back("-lc"); // Link against the C standard library

	// Use the glibc dynamic linker, this will need to be changed for other C libraries
	prog.args.push_back("-dynamic-linker=/lib64/ld-linux-x86-64.so.2");

	// Generate an eh_frame header, which allows runtime access to the exception information.
	// This is required for C++ to throw exceptions through our generated functions.
	prog.args.push_back("--eh-frame-hdr");

	switch (type) {
	case OutputType::OT_EXEC: {
		// Link it to the standalone programme stub
		prog.args.push_back(compilerInstallDir + "/wren-rtlib-stub");
		break;
	}
	case OutputType::OT_LIB_SHARED: {
		prog.args.push_back("-shared");
		break;
	}
	default:
		fprintf(stderr, "invalid linker output type %d\n", (int)type);
		abort();
	}

	prog.withEnv = true;
	prog.Run();
}

static void staticLink(const std::string &outputFile, const std::vector<std::string> &objectFiles) {
	// If the output file already exists, get rid of it since AR won't wipe the archive
	remove(outputFile.c_str());

	// Run AR to build the static archive and add an index
	RunProgramme prog;
	prog.args.push_back("ar");

	// q=quick append, we're not going to overwrite anything since this is a new archive
	// c=create, don't warn that we're making a new archive
	// s=index, equivalent of running ranlib
	prog.args.push_back("qcs");

	// Output file goes first, followed by the inputs
	prog.args.push_back(outputFile);
	prog.args.insert(prog.args.end(), objectFiles.begin(), objectFiles.end());

	prog.withEnv = true;
	prog.Run();
}
