//
// The testing tool for the IDE support system.
//
// This is for developing and testing the IDE support system. Actual editor
// integration (either directly or via something like LSP) will live outside
// of this file.
//
// Created by znix on 27/02/23.
//

#include "ide_devtool.h"
#include "ActiveFile.h"
#include "GrammarInfo.h"
#include "ide_util.h"

#include <assert.h>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

GrammarInfo *grammarInfo = nullptr;

int main(int argc, char **argv) {
	std::string filename = argv[1];

	// Create a parser.
	TSParser *parser = ts_parser_new();

	// Set the parser's language (JSON in this case).
	ts_parser_set_language(parser, tree_sitter_wren());

	// Build a syntax tree based on source code stored in a string.
	TSTree *tree;
	std::string fileContents;
	try {
		std::ifstream input;
		input.exceptions(std::ios::badbit | std::ios::failbit);
		input.open(filename);

		// Quick way to read an entire stream
		std::stringstream buf;
		buf << input.rdbuf();
		fileContents = buf.str();

		// Erase any assertions we find in the raw string, before parsing it.
		// See test/README.md for more details.
		while (true) {
			size_t pos = fileContents.find("¬");
			if (pos == std::string::npos)
				break;

			size_t endPos = fileContents.find('!', pos);
			assert(endPos != std::string::npos);

			// We need to make the range inclusive to remove
			// the trailing '!', so +1 it.
			fileContents.erase(pos, endPos - pos + 1);

			// Print out the offset of the assertion, to make interactive
			// use easier. Note this character-counting breaks with non-ASCII
			// characters, but it's mostly good enough for our testing.
			// Use pos-1 to reference the previous newline, in the case that
			// we deleted everything until the end of the current line.
			size_t lineStart = fileContents.rfind('\n', pos - 1);
			if (lineStart == std::string::npos)
				lineStart = 0;
			int column = pos - lineStart;

			// TODO put this behind a verbose flag
			ideDebug("Removing assertion at position %d, column %d", (int)pos, (int)column);
		}
		// ideDebug("Running on input:\n%s", fileContents.c_str());

		tree = ts_parser_parse_string(parser, NULL, fileContents.c_str(), fileContents.length());
	} catch (const std::fstream::failure &ex) {
		fprintf(stderr, "Failed to open input file: %s\n", ex.what());
		exit(1);
	}

	GrammarInfo gi;
	grammarInfo = &gi;

	ActiveFile file;
	file.Update(tree, fileContents);

	// Read all the commands from stdin, and reply to them.

	std::string line;
	while (std::getline(std::cin, line)) {
		// Parse out the line/column numbers
		int lineNum = 0, column = 0;
		char *cCommand = nullptr;
		int countConsumed = 0;
		if (sscanf(line.c_str(), "cmd:%d.%d:%m[^:]:%n", &lineNum, &column, &cCommand, &countConsumed) != 3) {
			fprintf(stderr, "Invalid command: '%s'\n", line.c_str());
			return 1;
		}
		if (countConsumed == 0) {
			fprintf(stderr, "Invalid command: '%s' - missing colon before argument\n", line.c_str());
			return 1;
		}

		std::string command = cCommand;
		free(cCommand);
		cCommand = nullptr;

		// Cut off the line/column numbers and command, leaving a command-specific argument.
		line.erase(0, countConsumed);

		TSPoint point = {(uint32_t)lineNum, (uint32_t)column};

		printf("========== COMMAND %d.%d %s\n", lineNum, column, command.c_str());

		if (command == "complete") {
			AutoCompleteResult result = file.AutoComplete(point);

			for (const AutoCompleteResult::Entry &entry : result.entries) {
				printf("entry %s\n", entry.identifier.c_str());
			}
		} else {
			fprintf(stderr, "Invalid command: '%s'\n", command.c_str());
			return 1;
		}
	}

	// Free the tree-sitter stuff.
	ts_tree_delete(tree);
	ts_parser_delete(parser);

	grammarInfo = nullptr;
	return 0;
}
