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

	static bool printCommands;

	std::vector<std::string> args;

	/// The data that should be sent to the process over stdin.
	std::string input;

	/// If true, the programme is run via the /usr/bin/env wrapper, to perform a PATH lookup.
	bool withEnv = false;

	void Run();

  private:
	// Put together a properly quoted string, for printing as a diagnostic
	// aid or for running on Windows where argument splitting is done at
	// process start :(
	std::string BuildCommandString();

	void PrintCommand();
};
