//
// Created by znix on 23/07/22.
//

#pragma once

#include <optional>
#include <string>
#include <vector>

class RunProgramme {
  public:
	RunProgramme();
	~RunProgramme();

	std::vector<std::string> args;

	/// A list of file descriptors to leave open and accessible from the child process.
	/// The intent is to use them with /dev/fd/x and open(O_TMPFILE).
	std::vector<int> preservedFDs;

	/// The data that should be sent to the process over stdin.
	std::string input;

	void Run();
};
