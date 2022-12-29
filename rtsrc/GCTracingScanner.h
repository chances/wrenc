//
// Created by znix on 24/12/22.
//

#pragma once

#include "Obj.h"
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

	void BeginGCCycle();

	/// Walk the current thread's stack, and find and mark any values in managed (Wren) code as roots
	void MarkCurrentThreadRoots();

	// Implement a simple tri-colour scanner: black is unreachable, grey is reachable but un-scanned, and
	// white is reachable and scanned.

	/// Mark a given value as a root. Passing in NULL_VAL or a number are both valid, but ignored.
	void MarkValueAsRoot(Value value);

	/// Walk the object graph using the previously-marked roots, and free everything that's not marked.
	void EndGCCycle();

  private:
	struct FunctionInfo {
		/// The address of this function
		void *functionPointer = nullptr;

		/// The stack map this function comes from
		StackMapDescription *stackMap = nullptr;
	};

	struct MarkOpsImpl : public GCMarkOps {
		GCTracingScanner *scanner;
	};

	/// Add an object to the grey list, if required. If nullptr is passed, it's ignored.
	void AddToGreyList(Obj *obj);

	/// The current number stored in the GC word of all objects marked white (reachable and scanned) in the
	/// tri-colour GC system. See the comment at the start of the cpp file for more information.
	uint32_t m_currentWhiteNumber = 0xabcd;

	/// The queue of nodes we've yet to scan: nodes should be added when they change from black to grey, and
	/// removed when they change from grey to white (the latter happens by popping a value and processing it).
	/// This is an invasive linked list, using the GC word encoded as a Value.
	Obj *m_greyList = nullptr;

	/// A sorted list of functions, so we can binary-search it to find what function might contain a given
	/// instruction pointer. This is used because functions from different modules might be mixed in with
	/// each other, and we need to find which module so we know which stackmap to query.
	std::vector<FunctionInfo> m_functions;

	/// Implementations of the GCMarkOps field functions
	static void OpsReportValue(GCMarkOps *thisObj, Value value);
	static void OpsReportValues(GCMarkOps *thisObj, const Value *values, int count);
	static void OpsReportObject(GCMarkOps *thisObj, Obj *object);
	static void OpsReportObjects(GCMarkOps *thisObj, Obj *const *objects, int count);
};
