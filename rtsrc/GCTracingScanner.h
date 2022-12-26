//
// Created by znix on 24/12/22.
//

#pragma once

#include "common.h"

#include <deque>
#include <vector>

class StackMapDescription;

class GCTracingScanner {
  public:
	GCTracingScanner();
	~GCTracingScanner();

	GCTracingScanner(const GCTracingScanner &) = delete;
	GCTracingScanner &operator=(const GCTracingScanner &) = delete;

	/// Walk the current thread's stack, and find and mark any values in managed (Wren) code as roots
	void MarkCurrentThreadRoots();

	// Implement a simple tri-colour scanner: black is unreachable, grey is reachable but un-scanned, and
	// white is reachable and scanned.

	/// Mark a given value as a root. Passing in NULL_VAL or a number are both valid, but ignored.
	void MarkValueAsRoot(Value value);

  private:
	struct FunctionInfo {
		/// The address of this function
		void *functionPointer = nullptr;

		/// The stack map this function comes from
		StackMapDescription *stackMap = nullptr;
	};

	/// The queue of nodes we've yet to scan: nodes should be added when they change from black to grey, and
	/// removed when they change from grey to white (the latter happens by popping a value and processing it).
	std::deque<Value> m_greyList;

	/// A sorted list of functions, so we can binary-search it to find what function might contain a given
	/// instruction pointer. This is used because functions from different modules might be mixed in with
	/// each other, and we need to find which module so we know which stackmap to query.
	std::vector<FunctionInfo> m_functions;
};
