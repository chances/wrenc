#include <fmt/format.h>

#include "IRNode.h"
#include "backend_qbe/QbeBackend.h"
#include "passes/IRCleanup.h"
#include "wren_compiler.h"

#include <sstream>

int main(int argc, char **argv) {
	fmt::print("hello world\n");

	const char *test = R"(
var hi = "abc"
System.print("Hello, %(hi)!")

System.print("Hello, world!")

class Wren {
  construct new() {}
  flyTo(city) {
    System.print("Flying to %(city) ")
  }
}

var temp = Wren.new()
//temp.flyTo("Köln") // Test unicode

/*
var adjectives = Fiber.new {
	["small", "clean", "fast"].each {|word| Fiber.yield(word) }
}

while (!adjectives.isDone) System.print(adjectives.call())
*/
)";

	CompContext ctx;
	Module mod;
	IRFn *fn = wrenCompile(&ctx, &mod, test, false);

	if (!fn)
		return 1;

	IRCleanup cleanup;
	cleanup.Process(fn);

	IRPrinter printer;
	printer.Process(fn);
	std::unique_ptr<std::stringstream> dbg = printer.Extract();
	fmt::print("AST/IR:\n{}\n", dbg->str());

	QbeBackend backend;
	backend.Generate(&mod);

	return 0;
}
