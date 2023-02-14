//
// Created by znix on 23/07/22.
//

#define WIN32_LEAN_AND_MEAN

#include "RunProgramme.h"
#include "Platform.h"

#include <fmt/format.h>
#include <set>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <Windows.h>
#else
#include <sys/wait.h>
#endif

struct ExecData {
	const std::vector<std::string> *args = nullptr;
	int stdinPipe = -1;
	bool useEnv;
};

bool RunProgramme::printCommands = false;

RunProgramme::RunProgramme() {}
RunProgramme::~RunProgramme() {}

#ifdef _WIN32

void RunProgramme::Run() {
	if (printCommands)
		PrintCommand();

	// We could redirect stdin with STARTF_USESTDHANDLES, but I can't be bothered.
	if (!input.empty()) {
		fmt::print(stderr, "Windows RunProgramme doesn't yet support stdin.\n");
		abort();
	}

	STARTUPINFOA startupInfo = {};
	PROCESS_INFORMATION procInfo = {};

	startupInfo.cb = sizeof(startupInfo);

	bool success = CreateProcessA(args.at(0).c_str(), nullptr, nullptr, nullptr, false, 0, nullptr, nullptr,
	    &startupInfo, &procInfo);
	if (!success) {
		std::string error = plat_util::getLastWindowsError();
		fmt::print(stderr, "Failed to create process '{}': {}\n", args.at(0), error);
		exit(1);
	}

	// Wait for the process to exit
	WaitForSingleObject(procInfo.hProcess, INFINITE);

	DWORD exitCode;
	if (!GetExitCodeProcess(procInfo.hProcess, &exitCode)) {
		std::string error = plat_util::getLastWindowsError();
		fmt::print(stderr, "Failed to read process error: {}\n", error);
		abort();
	}

	if (exitCode != 0) {
		fmt::print(stderr, "Programme {} failed with exit code {}\n", args.at(0), exitCode);
		exit(1);
	}

	// Clean up
	CloseHandle(procInfo.hProcess);
	CloseHandle(procInfo.hThread);
}

#else

static int execSubProgramme(void *voidArgs);

void RunProgramme::Run() {
	if (printCommands)
		PrintCommand();

	int stdinPipePair[2];
	if (pipe(stdinPipePair)) {
		fmt::print(stderr, "Failed to create stdin pipe: {} {}", args.at(0), errno, strerror(errno));
		exit(1);
	}

	ExecData data = {
	    .args = &args,
	    .stdinPipe = stdinPipePair[0], // The pipe's read end
	    .useEnv = withEnv,
	};

	// Pass the end of the stack buffer, since it grows downwards
	// It'd be better to use a properly-allocated stack, but this should work for now and is much more convenient
	static uint8_t stackBuf[4096];
	int pid = clone(execSubProgramme, stackBuf + sizeof(stackBuf), CLONE_VFORK | SIGCHLD, &data);
	if (pid == -1) {
		fmt::print(stderr, "Failed to clone sub-programme {}: error {} {}\n", args.at(0), errno, strerror(errno));
		exit(1);
	}

	// Since we're not going to read from the pipe, close our handle to the read end of it
	if (close(stdinPipePair[0])) {
		fmt::print(stderr, "Failed to close stdin read pipe (compiler side): error {} {}\n", errno, strerror(errno));
		exit(1);
	}

	// Write what we're supposed to send to stdin to it
	errno = 0; // Just be sure we don't print a stale errno
	size_t written = write(stdinPipePair[1], input.c_str(), input.size());
	if (written != (size_t)input.size()) {
		fmt::print(stderr, "Failed to fully write to sub-programme (wrote {}/{} bytes) {}: error {} {}\n", written,
		    input.size(), args.at(0), errno, strerror(errno));
		exit(1);
	}

	// Indicate stdin is done by closing it
	if (close(stdinPipePair[1])) {
		fmt::print(stderr, "Failed to close stdin write pipe (compiler side): error {} {}\n", errno, strerror(errno));
		exit(1);
	}

	// We shouldn't get interrupted, but just in case, try again until it works
	int deadPid;
	int status;
	do {
		deadPid = waitpid(pid, &status, 0);
	} while (deadPid == -1 && errno == EINTR);

	if (deadPid != pid) {
		fmt::print(stderr, "Failed to wait for sub-programme termination ({} vs {}) - error {} {}\n", deadPid, pid,
		    errno, strerror(errno));
		exit(1);
	}

	if (status) {
		fmt::print(stderr, "Programme {} failed with status code {}\n", args.at(0), status);
		exit(1);
	}
}

static int execSubProgramme(void *voidArgs) {
	const ExecData *data = (const ExecData *)voidArgs;

	// Use the piped data as our stdin
	if (dup2(data->stdinPipe, 0) == -1) {
		fmt::print("Failed to redirect stdin in child process: {} {}", errno, strerror(errno));
		return 1;
	}

	// Close all the files except stdin, stdout and stderr (FDs 0,1,2 respectively).
	// (well technically this doesn't guarantee 'all', but it's close enough)
	for (int i = 3; i < 1000; i++) {
		close(i);
	}

	// Prepare a null-terminated arguments array of null-terminated strings
	std::vector<char *> rawArgs;
	if (data->useEnv) {
		rawArgs.push_back(strdup("/usr/bin/env"));
	}
	for (const std::string &str : *data->args) {
		rawArgs.push_back(strdup(str.c_str())); // Strdup since the string needs to be mutable.
	}
	rawArgs.push_back(0); // Mark the end of the array

	execv(rawArgs.front(), rawArgs.data());
	fmt::print("Failed to spawn new process '{}': {} {}", data->args->at(0), errno, strerror(errno));
	return 1;
}

#endif

void RunProgramme::PrintCommand() {
	std::string result;

	for (std::string arg : args) {
		// First, check if there's anything in this string that would require it to be quoted
		bool needsQuotes = false;
		for (char c : arg) {
			if (c == ' ' || c == '|' || c == '$' || c == '\\' || c == '#')
				needsQuotes = true;
			if (c == '"' || c == '\'' || c == '`')
				needsQuotes = true;
		}

		result.reserve(result.size() + arg.size() + 10); // 10 for quotes, escapes etc

		if (!result.empty())
			result.push_back(' ');

		if (needsQuotes)
			result.push_back('"');

		for (char c : arg) {
			if (c == '"' || c == '\\')
				result.push_back('\\');
			result.push_back(c);
		}

		if (needsQuotes)
			result.push_back('"');
	}

	fmt::print("Running command: {}\n", result);
}
