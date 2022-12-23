//
// Created by znix on 23/07/22.
//

#include "RunProgramme.h"

#include <fmt/format.h>
#include <set>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>

static int execSubProgramme(void *voidArgs);

struct ExecData {
	const std::vector<std::string> *args = nullptr;
	const std::vector<int> *preservedFDs = nullptr;
	int stdinPipe = -1;
	int stdoutPipe = -1;
};

RunProgramme::RunProgramme() {}
RunProgramme::~RunProgramme() {}

void RunProgramme::Run() {
	int stdinPipePair[2];
	if (pipe(stdinPipePair)) {
		fmt::print(stderr, "Failed to create stdin pipe: {} {}", args.at(0), errno, strerror(errno));
		exit(1);
	}

	ExecData data = {
	    .args = &args,
	    .preservedFDs = &preservedFDs,
	    .stdinPipe = stdinPipePair[0], // The pipe's read end
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

	// Build a set of the FDs to keep around
	std::set<int> toKeep;
	for (int fd : *data->preservedFDs) {
		toKeep.insert(fd);
	}

	// Close all the files except stdin, stdout and stderr (FDs 0,1,2 respectively), and those we were told not to
	// (well technically this doesn't guarantee 'all', but it's close enough)
	for (int i = 3; i < 1000; i++) {
		if (!toKeep.contains(i))
			close(i);
	}

	// Prepare a null-terminated arguments array of null-terminated strings
	std::vector<char *> rawArgs;
	for (const std::string &str : *data->args) {
		rawArgs.push_back(strdup(str.c_str())); // Strdup since the string needs to be mutable.
	}
	rawArgs.push_back(0); // Mark the end of the array

	execv(rawArgs.front(), rawArgs.data());
	fmt::print("Failed to spawn new process '{}': {} {}", data->args->at(0), errno, strerror(errno));
	return 1;
}
