#include <fmt/format.h>

#include "IRNode.h"
#include "RunProgramme.h"
#include "backend_qbe/QbeBackend.h"
#include "passes/IRCleanup.h"
#include "wren_compiler.h"

#include <fcntl.h>
#include <fstream>
#include <signal.h>
#include <sstream>

static const std::string QBE_PATH = "./lib/qbe-1.0/qbe_bin";

static int runQbe(std::string qbeIr);
static void runAssembler(int assemblyFd, const std::string &outputFilename);
static void runLinker(const std::string &executableFile, const std::vector<std::string> &objectFiles);

static std::string compilerInstallDir;

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

	fmt::print("hello world\n");

	const char *test = R"(
var hi = "abc"
// System.print("Hello, %(hi)!")

System.print("Hello, world!")

class Wren {
  construct new() {}
  flyTo(city) {
    System.print("Flying to %(city) ")
  }

  static testStatic() {
    System.print("static test")
  }
}

Wren.testStatic()
var temp = Wren.new()
temp.flyTo("Köln") // Test unicode

for (adjective in ["small", "clean", "fast"]) {
  System.print(adjective)
}

/*
var adjectives = Fiber.new {
	["small", "clean", "fast"].each {|word| Fiber.yield(word) }
}

while (!adjectives.isDone) System.print(adjectives.call())
*/
)";

	CompContext ctx;
	Module mod;
	IRFn *rootFn = wrenCompile(&ctx, &mod, test, false);

	if (!rootFn)
		return 1;

	IRCleanup cleanup;
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

	QbeBackend backend;
	std::string qbeIr = backend.Generate(&mod);

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
	runAssembler(assemblyFileFd, "/tmp/wren-output-test.o");
	runLinker("/tmp/wren-output-test.exe", {"/tmp/wren-output-test.o"});

	return 0;
}

std::string filenameForFd(int fd) { return fmt::format("/dev/fd/{}", fd); }

static int runQbe(std::string qbeIr) {
	// Create a temporary file to store the output
	// This creates an unnamed, anonymous file that's deleted when the programme exists
	int tmpFd = open("/tmp", O_TMPFILE | O_EXCL | O_WRONLY, 0600);
	if (tmpFd == -1) {
		fmt::print(stderr, "Cannot crate temporary QBE assembly file");
	}

	// Run the programme
	RunProgramme prog;
	prog.args.push_back(QBE_PATH);
	prog.args.push_back("-o");
	prog.args.push_back(filenameForFd(tmpFd));
	prog.preservedFDs.push_back(tmpFd);
	prog.input = std::move(qbeIr);
	prog.Run();

	return tmpFd;
}

static void runAssembler(int assemblyFd, const std::string &outputFilename) {
	RunProgramme prog;
	prog.args.push_back("/usr/bin/env");
	prog.args.push_back("as");
	prog.args.push_back("-o");
	prog.args.push_back(outputFilename);
	prog.args.push_back(filenameForFd(assemblyFd));
	prog.preservedFDs.push_back(assemblyFd);
	prog.Run();
}

static void runLinker(const std::string &executableFile, const std::vector<std::string> &objectFiles) {
	RunProgramme prog;
	prog.args.push_back("/usr/bin/env");
	prog.args.push_back("ld.gold"); // Not everyone has lld, and gold is fast enough for now
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
