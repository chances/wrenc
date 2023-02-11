//
// Created by znix on 28/07/22.
//

// Only use 'local unwinding' - see https://www.nongnu.org/libunwind/man/libunwind(3).html
#define UNW_LOCAL_ONLY

#include "ObjFibre.h"
#include "Errors.h"
#include "GCTracingScanner.h"
#include "ObjBool.h"
#include "ObjClass.h"
#include "ObjFn.h"
#include "SlabObjectAllocator.h"

#include <libunwind.h>
#include <string.h>
#include <sys/mman.h>

// From the assembly functions
// Both of these actually return ResumeFibreArgs, since both only 'return' when switchToExisting
// is called.
extern "C" {
// NOLINTNEXTLINE(readability-identifier-naming)
WREN_MSVC_CALLCONV void *fibreAsm_invokeOnNewStack(void *newStack, void *func, void *arg);
// NOLINTNEXTLINE(readability-identifier-naming)
WREN_MSVC_CALLCONV void *fibreAsm_switchToExisting(void *newStack, /*ResumeFibreArgs*/ void *arg);
}

struct ObjFibre::StartFibreArgs {
	Value argument;
	ObjFibre *newFibre;
	ObjFibre *oldFibre;
};

struct ObjFibre::ResumeFibreArgs {
	void *oldStackPtr = nullptr; // Written to by assembly
	Value argument = NULL_VAL;
	ObjFibre *oldFibre = nullptr;

	/// If true then the fibre that just returned has finished executing, and
	/// it's stack should be destroyed.
	bool isTerminating = false;
};

int ObjFibre::stackSize = 0;
std::vector<ObjFibre *> ObjFibre::fibreCallStack;

class ObjFibreClass : public ObjNativeClass {
  public:
	ObjFibreClass() : ObjNativeClass("Fiber", "ObjFibre") {}
};

ObjFibre::~ObjFibre() {}
ObjFibre::ObjFibre() : Obj(Class()) {
	// This may be called by the GC while the fibre is suspended. In this
	// case, we need to delete the stack.
	DeleteStack();
}

ObjClass *ObjFibre::Class() {
	static ObjFibreClass cls;
	return &cls;
}

ObjFibre *ObjFibre::GetMainThreadFibre() {
	static ObjFibre *ptr = nullptr;
	if (ptr)
		return ptr;

	// Set up the fibre
	static ObjFibre fibre;
	ptr = &fibre;

	fibre.m_state = State::RUNNING;

	return ptr;
}

void ObjFibre::MarkGCValues(GCMarkOps *ops) {
	ops->ReportObject(ops, m_function);

	// If we're not suspended, we don't need to walk the stack:
	// * Running: stack is scanned by the GC automatically.
	// * Finished/not started/failed: there's no stack to scan.
	if (m_state != State::SUSPENDED) {
		return;
	}

	// Use the GC to mark all the values in the fibre's stack as roots. It's
	// fine to do this while the GC is walking the heap, everything just gets
	// added to the grey list anyway.
	GCTracingScanner *gc = (GCTracingScanner *)ops->GetGCImpl(ops);
	gc->MarkThreadRoots(m_suspendedContext);
}

ObjFibre *ObjFibre::New(ObjFn *func) {
	// If we're just starting up (and thus the fibre call stack is empty), then place the
	// main thread fibre on it. There has to be something there to be able to switch properly.
	if (fibreCallStack.empty()) {
		fibreCallStack.push_back(GetMainThreadFibre());
	}

	ObjFibre *fibre = SlabObjectAllocator::GetInstance()->AllocateNative<ObjFibre>();
	fibre->m_function = func;
	return fibre;
}

void ObjFibre::CheckStack() {
	if (m_stack)
		return;

	// Calculate the stack size, making sure it's aligned to the page size.
	// We'll used fixed-size stacks to make them easy to free, and because there's very
	// little cost in doing so. On Linux we can use MAP_GROWSDOWN to get
	// a dynamically-sized stack, but that's a pain to free and doesn't really have
	// any big concrete advantages.
	if (stackSize == 0) {
		int pageSize = getpagesize();
		stackSize = 2 * 1024 * 1024; // 2MiB stacks should be waaay more than enough
		int overhang = stackSize % pageSize;
		if (overhang) {
			stackSize += pageSize - overhang;
		}
	}

	m_stack = (void *)mmap(nullptr, stackSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
	if (m_stack == MAP_FAILED) {
		fprintf(stderr, "Failed to map new stack for fibre.\n");
		abort();
	}

	// TODO remove write permissions on the last page to catch overruns if there's something else mapped there, which
	//  is something I've observed during testing.
}

void ObjFibre::DeleteStack() {
	if (m_state == State::RUNNING) {
		fprintf(stderr, "ObjFibre: Cannot delete stack of running fibre.\n");
		abort();
	}

	if (m_stack == nullptr)
		return;

	if (munmap(m_stack, stackSize)) {
		fprintf(stderr, "ObjFibre: Failed to unmap stack of fibre: %d %s\n", errno, strerror(errno));
		abort();
	}

	m_stack = nullptr;
}

Value ObjFibre::Call() { return Call(NULL_VAL); }

Value ObjFibre::Call(Value argument) {
	ObjFibre *previous = fibreCallStack.back();
	fibreCallStack.push_back(this);

	if (m_state == State::NOT_STARTED) {
		return StartAndSwitchTo(previous, argument);
	}
	if (m_state == State::SUSPENDED) {
		return ResumeSuspended(previous, argument, false);
	}

	// TODO
	abort();
}

Value ObjFibre::Yield() { return Yield(NULL_VAL); }

Value ObjFibre::Yield(Value argument) {
	ObjFibre *oldFibre = fibreCallStack.back();
	fibreCallStack.pop_back();
	ObjFibre *newFibre = fibreCallStack.back();

	return newFibre->ResumeSuspended(oldFibre, argument, false);
}

Value ObjFibre::StartAndSwitchTo(ObjFibre *previous, Value argument) {
	if (m_state != State::NOT_STARTED) {
		fprintf(stderr, "Cannot call ObjFibre::StartAndSwitchTo on fibre in state %d\n", (int)m_state);
		abort();
	}

	CheckStack();

	StartFibreArgs args = {
	    .argument = argument,
	    .newFibre = this,
	    .oldFibre = previous,
	};

	previous->m_state = State::SUSPENDED;
	m_state = State::RUNNING;

	// Save the current context, so our stack can be unwound by the GC. It's
	// safe to put out a pointer to something on the stack, since it'll be
	// cleared by ResumeSuspended before switchToExisting returns.
	// TODO deduplicate with ResumeSuspended.
	unw_context_t context;
	unw_getcontext(&context);
	previous->m_suspendedContext = &context;

	// Find the top of the stack - that's what we pass to the assembly, since we work downwards that's a lot
	// more useful.
	void *topOfStack = (void *)((uint64_t)m_stack + stackSize);

	ResumeFibreArgs *result = (ResumeFibreArgs *)fibreAsm_invokeOnNewStack(topOfStack, (void *)RunOnNewStack, &args);
	return HandleResumed(result);
}

Value ObjFibre::ResumeSuspended(ObjFibre *previous, Value argument, bool terminate) {
	if (m_state != State::SUSPENDED) {
		fprintf(stderr, "Cannot call ObjFibre::ResumeSuspended on fibre in state %d\n", (int)m_state);
		abort();
	}

	ResumeFibreArgs args = {
	    .oldStackPtr = nullptr,
	    .argument = argument,
	    .oldFibre = previous,
	    .isTerminating = terminate,
	};
	previous->m_state = State::SUSPENDED;
	m_state = State::RUNNING;

	void *resumeStackAddr = m_resumeAddress;

	// Save the current context, so our stack can be unwound by the GC. It's
	// safe to put out a pointer to something on the stack, since it'll be
	// cleared by ResumeSuspended before switchToExisting returns.
	unw_context_t context;
	unw_getcontext(&context);
	previous->m_suspendedContext = &context;

	// Clear out our old suspended pointers, since we're about to start running
	// they're going to be invalid.
	m_resumeAddress = nullptr;
	m_suspendedContext = nullptr;

	ResumeFibreArgs *result = (ResumeFibreArgs *)fibreAsm_switchToExisting(resumeStackAddr, &args);
	return HandleResumed(result);
}

Value ObjFibre::HandleResumed(ObjFibre::ResumeFibreArgs *result) {
	// We've now come back from some arbitrary fibre, store away it's return address.
	ObjFibre *fibre = result->oldFibre;
	fibre->m_resumeAddress = result->oldStackPtr;

	// Grab the argument now, since we might free the stack this struct lives on
	Value arg = result->argument;

	if (result->isTerminating) {
		fibre->m_state = State::FINISHED;
		fibre->m_function = nullptr; // Allow the function to be GCed.
		fibre->DeleteStack();
		// DO NOT ACCESS RESULT AFTER THIS POINT - IT HAS BEEN FREED!
	}

	return arg;
}

WREN_MSVC_CALLCONV void ObjFibre::RunOnNewStack(void *oldStack, StartFibreArgs *incomingArgs) {
	// Copy the args - they'll become invalid since they're allocated on another stack
	StartFibreArgs args = *incomingArgs;
	args.oldFibre->m_resumeAddress = oldStack;

	ObjFn *fn = args.newFibre->m_function;
	Value result;
	if (fn->spec->arity == 0) {
		result = fn->Call({});
	} else {
		result = fn->Call({args.argument});
	}

	assert(fibreCallStack.back() == args.newFibre);
	fibreCallStack.pop_back();
	ObjFibre *next = fibreCallStack.back();
	next->ResumeSuspended(args.newFibre, result, true);

	fprintf(stderr, "Resumed thread that should be destroyed!\n");
	abort();
}

void ObjFibre::Abort(std::string message) {
	// TODO implement fibre abortion properly
	errors::wrenAbort("%s", message.c_str());
}

Value ObjFibre::IsDone() {
	bool finished = m_state == State::FINISHED || m_state == State::FAILED;
	return encode_object(ObjBool::Get(finished));
}

Value ObjFibre::Error() {
	// Errors aren't yet supported, but when they are, return the string
	// representing the error message.
	return NULL_VAL;
}
