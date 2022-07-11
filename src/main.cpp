#include <fmt/format.h>

#include "wren_compiler.h"

int main(int argc, char **argv) {
	fmt::print("hello world\n");
	wrenCompile(nullptr, nullptr, nullptr, true);
}
