//
// Created by znix on 20/12/22.
//

#include "Utils.h"

#include <unistd.h>
#include <vector>

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
		filename.at(i) = chars.at(rand() % chars.size());
	}

	tempFiles.push_back(filename);
	return filename;
}
