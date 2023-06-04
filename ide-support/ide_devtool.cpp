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
		// Remove all carriage returns, so we don't have to deal with newlines twice.
		for (int i = line.size() - 1; i >= 0; i--) {
			if (line[i] == '\r') {
				line.erase(i);
			}
		}

		// Parse out the line/column numbers
		int lineNum = 0, column = 0;
		std::vector<char> commandChars(line.size());
		int countConsumed = 0;
		if (sscanf(line.c_str(), "cmd:%d.%d:%[^:]:%n", &lineNum, &column, commandChars.data(), &countConsumed) != 3) {
			fprintf(stderr, "Invalid command: '%s'\n", line.c_str());
			return 1;
		}
		if (countConsumed == 0) {
			fprintf(stderr, "Invalid command: '%s' - missing colon before argument\n", line.c_str());
			return 1;
		}

		std::string command = commandChars.data();

		// Cut off the line/column numbers and command, leaving a command-specific argument.
		line.erase(0, countConsumed);

		TSPoint point = {(uint32_t)lineNum, (uint32_t)column};

		printf("========== COMMAND %d.%d %s\n", lineNum, column, command.c_str());

		if (command == "complete") {
			AutoCompleteResult result = file.AutoComplete(point);

			for (const AutoCompleteResult::Entry &entry : result.entries) {
				printf("entry %s\n", entry.identifier.c_str());
			}
		} else if (command == "edit") {
			int eraseCount;
			int cmdOffset = 0;
			sscanf(line.c_str(), "%d:%n", &eraseCount, &cmdOffset);
			if (cmdOffset == 0) {
				fprintf(stderr, "Invalid edit command!\n");
				return 1;
			}

			// The input string is percent-encoded so it can be passed in one line.
			std::string replacement;
			replacement.reserve(line.size());
			for (int i = cmdOffset; i < (int)line.size(); i++) {
				char c = line.at(i);
				if (c == '%') {
					char c1 = line.at(++i);
					char c2 = line.at(++i);

					// Do the most horrible hex decode, which only handles
					// capitals and doesn't have any kind of error handling.
					int upper = c1 >= 'A' ? (c1 - 'A' + 10) : c1 - '0';
					int lower = c2 >= 'A' ? (c2 - 'A' + 10) : c2 - '0';
					replacement.push_back((upper << 4) | lower);
				} else {
					replacement.push_back(c);
				}
			}

			// Find the appropriate line offset
			size_t offset = 0;
			for (int i = 0; i < lineNum; i++) {
				offset = fileContents.find('\n', offset);
				if (offset == std::string::npos) {
					fprintf(stderr, "Too high line number for command '%s'\n", command.c_str());
					return 1;
				}

				// We found the newline, advance by one to get to the start
				// of the next line.
				offset++;
			}

			// Add on the column number to find the byte at the start of the
			// sequence we want to edit.
			offset += column;

			// Perform the edit.
			fileContents.erase(offset, eraseCount);
			fileContents.insert(offset, replacement);

			// Update the file with the results of the edit.
			ts_tree_delete(tree);
			tree = ts_parser_parse_string(parser, NULL, fileContents.c_str(), fileContents.length());
			file.Update(tree, fileContents);
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
