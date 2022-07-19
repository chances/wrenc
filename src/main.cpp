#include <fmt/format.h>

#include "IRNode.h"
#include "wren_compiler.h"

#include <sstream>

int main(int argc, char **argv) {
	fmt::print("hello world\n");

	const char *test = R"(
var hi = "abc"
)";

	CompContext ctx;
	Module mod;
	IRFn *fn = wrenCompile(&ctx, &mod, test, false);

	IRPrinter printer;
	printer.Process(fn);
	std::unique_ptr<std::stringstream> dbg = printer.Extract();
	fmt::print("AST/IR:\n{}\n", dbg->str());
}
