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
#include "ObjString.h"
#include "SlabObjectAllocator.h"
#include "WrenRuntime.h"

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
ObjFibre *ObjFibre::currentFibre = nullptr;

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
	ops->ReportObject(ops, m_parent);

	// If we're not suspended, we don't need to walk the stack:
	// * Running: stack is scanned by the GC automatically.
	// * Finished/not started/failed: there's no stack to scan.
	if (!IsSuspended()) {
		return;
	}

	// Use the GC to mark all the values in the fibre's stack as roots. It's
	// fine to do this while the GC is walking the heap, everything just gets
	// added to the grey list anyway.
	GCTracingScanner *gc = (GCTracingScanner *)ops->GetGCImpl(ops);
	gc->MarkThreadRoots(m_suspendedContext);
}

ObjFibre *ObjFibre::New(ObjFn *func) {
	// You can't pass more than one parameter to call(), so anything else will never
	// be usable.
	if (func->spec->arity > 1) {
		errors::wrenAbort("Function cannot take more than one parameter.");
	}

	// If we're just starting up (and thus the fibre call stack is empty), then place the
	// main thread fibre on it. There has to be something there to be able to switch properly.
	// Current() does this, so just use it.
	Current();

	ObjFibre *fibre = SlabObjectAllocator::GetInstance()->AllocateNative<ObjFibre>();
	fibre->m_function = func;
	return fibre;
}

ObjFibre *ObjFibre::Current() {
	if (currentFibre == nullptr) {
		currentFibre = GetMainThreadFibre();
	}
	return currentFibre;
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
	Value result = CallImpl(argument, false, false);

	// Exceptions propagate through fibres
	if (m_exception) {
		Abort(m_exception->message);
	}

	return result;
}

Value ObjFibre::Try() { return Try(NULL_VAL); }

Value ObjFibre::Try(Value argument) {
	Value result = CallImpl(argument, true, false);

	// Exceptions return their error value
	if (m_exception) {
		return m_exception->message;
	}

	return result;
}

Value ObjFibre::Transfer() { return Transfer(NULL_VAL); }

Value ObjFibre::Transfer(Value argument) {
	// Transferring to ourselves effectively just returns the
	// value passed in. Check for this specifically because our
	// standard fibre-switching system can't handle it.
	if (Current() == this) {
		return argument;
	}

	// Transferring puts this fibre into the same SUSPENDED state
	// as yielding, so it can later be transferred back to.
	Value result = CallImpl(argument, false, true);

	// We shouldn't have any problems with errors here, as
	// TransferError handles that itself (to avoid also dealing
	// with the case of transferError-ing a non-started fibre).

	return result;
}

Value ObjFibre::TransferError(Value message) {
	switch (m_state) {
	case State::NOT_STARTED:
	case State::SUSPENDED:
		break;
	case State::RUNNING:
	case State::WAITING:
		errors::wrenAbort("Fiber has already been called.");
	case State::FINISHED:
		errors::wrenAbort("Cannot transfer to a finished fiber.");
	case State::FAILED:
		errors::wrenAbort("Cannot transfer to an aborted fiber.");
	}

	// Here, we simulate the behaviour of resuming the fibre and immediately
	// sending an abort. This is faster and (more importantly) easier than
	// triggering an abort once we resume the other fibre.
	currentFibre = this;

	// Mark the fibre as failed.
	m_exception = std::make_unique<FibreAbortException>();
	m_exception->message = message;
	// TODO find the module
	m_state = State::FAILED;

	// Clean up this fibre, it won't be used again.
	// (Note that DeleteStack will be called by HandleResume once
	//  we're running on a different stack.)
	m_resumeAddress = nullptr;
	m_suspendedContext = nullptr;

	// Switch to it's parent
	if (m_parent == nullptr) {
		WrenRuntime::Instance().LastFibreExited(Obj::ToString(message));
	}
	m_parent->ResumeSuspended(NULL_VAL, true);

	fprintf(stderr, "Resumed thread that should be destroyed (via transferError)\n");
	abort();
}

Value ObjFibre::CallImpl(Value argument, bool isTry, bool isTransfer) {
	// In regular Wren you're not allowed to call the root fibre, which
	// is just fine for our implementation.
	if (this == GetMainThreadFibre() && !isTransfer) {
		errors::wrenAbort("Cannot call root fiber.");
	}

	auto setupState = [this, isTransfer]() {
		// If this is a transfer, don't set the parent. Transferring
		// back to a fibre that was transferred away from keeps it's
		// parent, so there can effectively be multiple 'stacks' of
		// fibres that are transferred between.
		if (!isTransfer)
			m_parent = currentFibre;

		// Transfers mark their fibre as suspended, so they can be transferred
		// back into.
		currentFibre->m_state = isTransfer ? State::SUSPENDED : State::WAITING;
	};

	switch (m_state) {
	case State::NOT_STARTED:
		setupState();
		return StartAndSwitchTo(argument);
	case State::SUSPENDED:
		setupState();
		return ResumeSuspended(argument, false);
	case State::RUNNING:
	case State::WAITING:
		errors::wrenAbort("Fiber has already been called.");
	case State::FINISHED:
		if (isTransfer) {
			errors::wrenAbort("Cannot transfer to a finished fiber.");
		} else if (isTry) {
			errors::wrenAbort("Cannot try a finished fiber.");
		} else {
			errors::wrenAbort("Cannot call a finished fiber.");
		}
	case State::FAILED:
		if (isTransfer) {
			errors::wrenAbort("Cannot transfer to an aborted fiber.");
		} else if (isTry) {
			errors::wrenAbort("Cannot try an aborted fiber.");
		} else {
			errors::wrenAbort("Cannot call an aborted fiber.");
		}
	}
	assert(0 && "Invalid fibre state");
}

Value ObjFibre::Yield() { return Yield(NULL_VAL); }

Value ObjFibre::Yield(Value argument) {
	// Initialise currentFibre if it's currently null
	Current();

	ObjFibre *oldFibre = currentFibre;
	oldFibre->m_state = State::SUSPENDED;

	// If we're yielding from a fibre with no parents, that ends execution
	if (oldFibre->m_parent == nullptr) {
		WrenRuntime::Instance().LastFibreExited(std::nullopt);
	}

	return oldFibre->m_parent->ResumeSuspended(argument, false);
}

Value ObjFibre::StartAndSwitchTo(Value argument) {
	if (m_state != State::NOT_STARTED) {
		fprintf(stderr, "Cannot call ObjFibre::StartAndSwitchTo on fibre in state %d\n", (int)m_state);
		abort();
	}

	CheckStack();

	StartFibreArgs args = {
	    .argument = argument,
	    .newFibre = this,
	    .oldFibre = currentFibre,
	};

	m_state = State::RUNNING;

	// Save the current context, so our stack can be unwound by the GC. It's
	// safe to put out a pointer to something on the stack, since it'll be
	// cleared by ResumeSuspended before switchToExisting returns.
	// TODO deduplicate with ResumeSuspended.
	unw_context_t context;
	unw_getcontext(&context);
	currentFibre->m_suspendedContext = &context;

	// Find the top of the stack - that's what we pass to the assembly, since we work downwards that's a lot
	// more useful.
	void *topOfStack = (void *)((uint64_t)m_stack + stackSize);

	currentFibre = this;
	ResumeFibreArgs *result = (ResumeFibreArgs *)fibreAsm_invokeOnNewStack(topOfStack, (void *)RunOnNewStack, &args);
	return HandleResumed(result);
}

Value ObjFibre::ResumeSuspended(Value argument, bool terminate) {
	if (!IsSuspended()) {
		fprintf(stderr, "Cannot call ObjFibre::ResumeSuspended on fibre in state %d\n", (int)m_state);
		abort();
	}

	ResumeFibreArgs args = {
	    .oldStackPtr = nullptr,
	    .argument = argument,
	    .oldFibre = currentFibre,
	    .isTerminating = terminate,
	};
	m_state = State::RUNNING;

	void *resumeStackAddr = m_resumeAddress;

	// Save the current context, so our stack can be unwound by the GC. It's
	// safe to put out a pointer to something on the stack, since it'll be
	// cleared by ResumeSuspended before switchToExisting returns.
	unw_context_t context;
	unw_getcontext(&context);
	currentFibre->m_suspendedContext = &context;

	// Clear out our old suspended pointers, since we're about to start running
	// they're going to be invalid.
	m_resumeAddress = nullptr;
	m_suspendedContext = nullptr;

	currentFibre = this;
	ResumeFibreArgs *result = (ResumeFibreArgs *)fibreAsm_switchToExisting(resumeStackAddr, &args);
	return HandleResumed(result);
}

Value ObjFibre::HandleResumed(ObjFibre::ResumeFibreArgs *result) {
	// We've now come back from some arbitrary fibre, store away it's return address.
	ObjFibre *fibre = result->oldFibre;
	fibre->m_resumeAddress = result->oldStackPtr;

	// There's no need to set currentFibre, as the ResumeSuspend call that resumed
	// the current fibre already did that for us.

	// Grab the argument now, since we might free the stack this struct lives on
	Value arg = result->argument;

	if (result->isTerminating) {
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
	// Using exceptions here *should* be safe, since we don't cross a stack while unwinding.
	try {
		if (fn->spec->arity == 0) {
			result = fn->Call({});
		} else {
			result = fn->Call({args.argument});
		}
		args.newFibre->m_state = State::FINISHED;
	} catch (const FibreAbortException &ex) {
		args.newFibre->m_exception = std::make_unique<FibreAbortException>(ex);
		args.newFibre->m_state = State::FAILED;
		result = NULL_VAL;
	}

	assert(currentFibre == args.newFibre);
	ObjFibre *next = args.newFibre->m_parent;
	if (next == nullptr) {
		std::optional<std::string> errorMessage;
		if (args.newFibre->m_exception)
			errorMessage = Obj::ToString(args.newFibre->m_exception->message);
		WrenRuntime::Instance().LastFibreExited(errorMessage);
	}
	next->ResumeSuspended(result, true);

	fprintf(stderr, "Resumed thread that should be destroyed!\n");
	abort();
}

void ObjFibre::Abort(Value message) {
	// A null message is a no-op.
	if (message == NULL_VAL)
		return;

	throw FibreAbortException{
	    .message = message,
	    // TODO find the module
	};
}

Value ObjFibre::IsDone() {
	bool finished = m_state == State::FINISHED || m_state == State::FAILED;
	return encode_object(ObjBool::Get(finished));
}

Value ObjFibre::Error() {
	if (!m_exception)
		return NULL_VAL;
	return m_exception->message;
}

bool ObjFibre::IsSuspended() const { return m_state == State::SUSPENDED || m_state == State::WAITING; }
