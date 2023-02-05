//
// Created by znix on 20/12/22.
//

#include "Utils.h"

#include <unistd.h>
#include <vector>
#include <random>

static std::vector<std::string> tempFiles;

static void onExitHandler() {
	for (const std::string &name : tempFiles) {
		if (unlink(name.c_str())) {
			fprintf(stderr, "Failed to delete temporary file %s\n", name.c_str());
		}
	}
}

static void setupAtExit() {
	static bool initDone = false;
	if (initDone)
		return;
	initDone = true;

	atexit(onExitHandler);
}

std::string utils::buildTempFilename(std::string nameTemplate) {
	setupAtExit();

	std::string filename = "/tmp/" + nameTemplate;

	// Use a proper seeded RNG
	static std::default_random_engine generator;
	static bool seeded = false;
	if (!seeded) {
		std::random_device trueRandom;
		generator.seed(trueRandom());
		seeded = true;
	}

	// FIXME come up with some better way of dealing with temporary files
	std::string chars;
	for (int i = 0; i < 10; i++) {
		chars.push_back('0' + i);
	}
	for (int i = 0; i < 26; i++) {
		chars.push_back('a' + i);
		chars.push_back('A' + i);
	}
	for (size_t i = 0; i < filename.size(); i++) {
		if (filename.at(i) != '!')
			continue;
		filename.at(i) = chars.at(generator() % chars.size());
	}

	tempFiles.push_back(filename);
	return filename;
}

std::string utils::stringFindReplace(std::string str, const std::string &from, const std::string &to) {
	size_t lastPos = 0;
	while (true) {
		lastPos = str.find(from, lastPos);
		if (lastPos == std::string::npos)
			return str;
		str.replace(lastPos, from.size(), to);
	}
}

std::vector<std::string> utils::stringSplit(const std::string &str, const std::string &sep) {
	std::vector<std::string> parts;

	size_t startPos = 0;
	while (true) {
		size_t pos = str.find(sep, startPos);
		if (pos == std::string::npos)
			break;

		// The section from startPos until (not including) pos is the next section
		parts.emplace_back(str.substr(startPos, pos - startPos));

		// Start immediately after the separator, otherwise we'll get stuck and infinitely loop on it
		startPos = pos + 1;
	}

	// Add the last part
	parts.emplace_back(str.substr(startPos));

	return parts;
}

std::string utils::stringJoin(const std::vector<std::string> &parts, const std::string &sep) {
	std::string result;

	bool isFirst = true;
	for (const std::string &str : parts) {
		if (!isFirst) {
			result.append(sep);
		}
		isFirst = false;

		result.append(str);
	}

	return result;
}
