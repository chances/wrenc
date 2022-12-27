#include <fmt/format.h>

#include "IRNode.h"
#include "RunProgramme.h"
#include "backend_llvm/LLVMBackend.h"
#include "backend_qbe/QbeBackend.h"
#include "passes/IRCleanup.h"
#include "wren_compiler.h"

#include <fcntl.h>
#include <fstream>
#include <getopt.h>
#include <signal.h>
#include <sstream>

static const std::string QBE_PATH = "lib/qbe-1.0/qbe_bin";

std::string filenameForFd(int fd);
static int runQbe(std::string qbeIr);
static CompilationResult runCompiler(const std::istream &input, const std::optional<std::string> &moduleName,
    const std::optional<std::string> &sourceFileName, bool main, const CompilationOptions *opts);
static void runAssembler(const std::vector<int> &assemblyFDs, const std::string &outputFilename);
static void runLinker(const std::string &executableFile, const std::vector<std::string> &objectFiles);

static std::string compilerInstallDir;

static int globalDontAssemble = 0;
static int globalBuildCoreLib = 0;
static int globalNoDebugInfo = 0;

static option options[] = {
    {"output", required_argument, 0, 'o'},
    {"compile-only", no_argument, 0, 'c'},
    {"module", required_argument, 0, 'm'},
    {"dont-assemble", no_argument, &globalDontAssemble, true},
    {"help", no_argument, 0, 'h'},
    {"no-debug-info", no_argument, &globalNoDebugInfo, true},

    // Intentionally undocumented options
    {"internal-build-core-lib", no_argument, &globalBuildCoreLib, true}, // Build the wren_core module

    {0},
};

int main(int argc, char **argv) {
	// Ignore SIGPIPE that might be caused if QBE or the assembler (run as child processes) crash - we can handle
	// the error and generate a proper error message, so turn this off.
	signal(SIGPIPE, SIG_IGN);

	// Find the directory of our executable, as we'll need libraries from the same directory later.
	{
		std::vector<char> buf;
		buf.resize(1024);
		if (readlink("/proc/self/exe", buf.data(), buf.size()) == -1) {
			fmt::print("Failed to read executable path: {} {}\n", errno, strerror(errno));
			exit(1);
		}

		// Chop off the executable name, leaving just the path
		compilerInstallDir = std::string(buf.data(), buf.size());
		size_t strokePos = compilerInstallDir.find_last_of('/');
		compilerInstallDir.erase(strokePos);
	}

	bool needsHelp = false; // Print the help page?
	bool compileOnly = false;
	std::string outputFile;
	std::vector<std::string> moduleNames;

	CompilationOptions backendOpts;

	while (true) {
		int opt = getopt_long(argc, argv, "o:m:ch", options, nullptr);

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

	if (needsHelp) {
		fmt::print("Usage: {} [-hc] [-o «filename»] inputs...\n", argv[0]);

		std::vector<std::tuple<std::string, std::string, std::string>> optHelp;
		optHelp.emplace_back("-h", "--help", "Print this help page");
		optHelp.emplace_back("-c", "--compile-only", "Only compile the input files, do not link them");
		optHelp.emplace_back("-o file", "--output=file", "Write the output to the file [file]");
		optHelp.emplace_back("-m name", "--module=name", "Sets the module name. Repeat for multiple files.");

		optHelp.emplace_back("", "--dont-assemble", "Write out an assembly file");
		optHelp.emplace_back("", "--no-debug-info", "Don't include any debugging information");
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

	std::vector<int> assemblyFiles;
	bool hitError = false;
	for (size_t sourceFileId = 0; sourceFileId < sourceFiles.size(); sourceFileId++) {
		const std::string &sourceFile = sourceFiles.at(sourceFileId);
		std::optional<std::string> thisModuleName;
		if (sourceFileId < moduleNames.size()) {
			thisModuleName = moduleNames.at(sourceFileId);
		}
		std::ifstream input;
		try {
			input.exceptions(std::ios::badbit | std::ios::failbit);
			input.open(sourceFile);
			// TODO add a proper way of selecting the main module
			CompilationResult result = runCompiler(input, thisModuleName, sourceFile, sourceFileId == 0, &backendOpts);
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
				if (result.fd != -1) {
					assemblyFiles.push_back(result.fd);
				} else if (!result.tempFilename.empty()) {
					int fd = open(result.tempFilename.c_str(), O_RDONLY);
					if (fd == -1) {
						fmt::print(stderr, "Cannot open temporary file {}: {} {}\n", sourceFile, errno,
						    strerror(errno));
						exit(1);
					}
					assemblyFiles.push_back(fd);
				} else {
					fmt::print(stderr, "Cannot process non-FD assembly compilation result for {}\n", sourceFile);
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
			for (int assemblyFd : assemblyFiles) {
				std::ifstream input;
				input.exceptions(std::ios::badbit | std::ios::failbit);
				input.open(filenameForFd(assemblyFd));
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
		runLinker(outputFile, objectFiles);
	}
}

static CompilationResult runCompiler(const std::istream &input, const std::optional<std::string> &moduleName,
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

	// Use the absolute path to the source file, so it works wherever it's run (on the same system, at least).
	if (sourceFileName) {
		char *resolvedC = realpath(sourceFileName.value().c_str(), nullptr);
		if (!resolvedC) {
			fmt::print(stderr, "Failed to resolve source file name - error {} {}!\n", errno, strerror(errno));
			exit(1);
		}
		mod.sourceFilePath = resolvedC;
		free(resolvedC);
	}

	IRFn *rootFn = wrenCompile(&ctx, &mod, source.c_str(), false, globalBuildCoreLib);

	if (!rootFn)
		return CompilationResult{.successful = false};

	IRCleanup cleanup(&ctx.alloc);
	for (IRFn *fn : mod.GetFunctions())
		cleanup.Process(fn);

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
		fmt::print(stderr, "Failed to write QBE IR: {}\n", ex.what());
		exit(1);
	}

	std::unique_ptr<IBackend> backend;
	const char *envUseLLVM = getenv("USE_LLVM");
	if (envUseLLVM && envUseLLVM == std::string("1")) {
#ifdef USE_LLVM
		backend = LLVMBackend::Create();
#else
		fmt::print(stderr, "LLVM support is not included in this build, ensure Meson's use_llvm is enabled\n");
		exit(1);
#endif
	} else {
		backend = std::unique_ptr<IBackend>(new QbeBackend());
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

	if (result.fd != -1) {
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

	int assemblyFileFd = runQbe(qbeIr);
	return CompilationResult{
	    .successful = true,
	    .fd = assemblyFileFd,
	    .format = CompilationResult::ASSEMBLY,
	};
}

std::string filenameForFd(int fd) { return fmt::format("/dev/fd/{}", fd); }

static int runQbe(std::string qbeIr) {
	// Create a temporary file to store the output
	// This creates an unnamed, anonymous file that's deleted when the programme exists
	int tmpFd = open("/tmp", O_TMPFILE | O_EXCL | O_WRONLY, 0600);
	if (tmpFd == -1) {
		fmt::print(stderr, "Cannot crate temporary QBE assembly file");
	}

	// Find the path to QBE, relative to this executable
	std::string qbePath = compilerInstallDir + "/" + QBE_PATH;

	// Run the programme
	RunProgramme prog;
	prog.args.push_back(qbePath);
	prog.args.push_back("-o");
	prog.args.push_back(filenameForFd(tmpFd));
	prog.preservedFDs.push_back(tmpFd);
	prog.input = std::move(qbeIr);
	prog.Run();

	return tmpFd;
}

static void runAssembler(const std::vector<int> &assemblyFDs, const std::string &outputFilename) {
	RunProgramme prog;
	prog.args.push_back("/usr/bin/env");
	prog.args.push_back("as");
	prog.args.push_back("-o");
	prog.args.push_back(outputFilename);
	for (int fd : assemblyFDs) {
		prog.args.push_back(filenameForFd(fd));
		prog.preservedFDs.push_back(fd);
	}
	prog.Run();
}

static void runLinker(const std::string &executableFile, const std::vector<std::string> &objectFiles) {
	RunProgramme prog;
	prog.args.push_back("/usr/bin/env");
	prog.args.push_back("ld"); // Using gold or lld would be good, but -lc seems to look for i386 libs on Gentoo/gold?
	prog.args.push_back("-o");
	prog.args.push_back(executableFile);

	// Link it to the standalone programme stub and the runtime library
	prog.args.push_back(compilerInstallDir + "/wren-rtlib-stub");
	prog.args.push_back(compilerInstallDir + "/libwren-rtlib.so");

	// Tell the dynamic link loader where to find the runtime library
	// TODO handle rtlib in some non-hardcoding-paths way on release builds
	prog.args.push_back("-rpath=" + compilerInstallDir);

	prog.args.insert(prog.args.end(), objectFiles.begin(), objectFiles.end());
	prog.args.push_back("-lc"); // Link against the C standard library

	// Use the glibc dynamic linker, this will need to be changed for other C libraries
	prog.args.push_back("-dynamic-linker=/lib64/ld-linux-x86-64.so.2");

	prog.Run();
}
