//
// Created by znix on 28/07/22.
//

#pragma once

#include "Obj.h"

#include <vector>

class ObjFn;
class GCTracingScanner;

// Whenever we're interacting with assembly we'll use the Microsoft calling convention, to avoid
// writing bits of the assembly twice.
#define WREN_MSVC_CALLCONV __attribute__((ms_abi))

/**
 * A fibre is a thread of execution in Wren. Fibres can't execute concurrently, but they can be interwoven to
 * do stuff like making a stream of tokens (this example is from the Wren homepage):
 *
 * \code
 *   var adjectives = Fiber.new {
 *     ["small", "clean", "fast"].each {|word| Fiber.yield(word) }
 *   }
 *   while (!adjectives.isDone) System.print(adjectives.call())
 * \endcode
 *
 * There's effectively a 'stack of fibres': one fibre can call another fibre, which yields a value back to the
 * first fibre.
 *
 * Fibres here will have their own full callstack. Since many fibres are likely to be short-lived, we'll pool their
 * callstacks to avoid the number of times we have to allocate and deallocate callstacks.
 *
 * Note this is spelt with the American spelling ('Fiber') in the API, but with the British spelling in C++ (it's
 * slightly confusing, but I'm writing this and I get to use the dialect I like :P ).
 */
class ObjFibre : public Obj {
  public:
	enum class State {
		NOT_STARTED,
		RUNNING,
		SUSPENDED,
		FINISHED,
		FAILED,
	};

	~ObjFibre();
	ObjFibre();

	static ObjClass *Class();

	static ObjFibre *GetMainThreadFibre();

	void MarkGCValues(GCMarkOps *ops) override;

	WREN_METHOD() static ObjFibre *New(ObjFn *func);

	WREN_METHOD() static Value Yield();
	WREN_METHOD() static Value Yield(Value argument);

	WREN_METHOD() static void Abort(std::string errorMessage);

	WREN_METHOD() Value Call();
	WREN_METHOD() Value Call(Value argument);

	WREN_METHOD(getter) Value IsDone();
	WREN_METHOD(getter) Value Error();

  private:
	struct StartFibreArgs;
	struct ResumeFibreArgs;

	/// If the stack isn't created, create it.
	void CheckStack();

	/// If we have a stack, delete it.
	void DeleteStack();

	/// If this is a brand-new fibre (NOT_STARTED), then create it's stack and jump to it.
	Value StartAndSwitchTo(ObjFibre *previous, Value argument);

	/// If this is a fibre waiting to continue (SUSPENDED), then jump back to it's stack
	/// and continue running it.
	Value ResumeSuspended(ObjFibre *previous, Value argument, bool terminate);

	/// Handle a fibre being resumed. Used by both StartAndSwitchTo and ResumeSuspended,
	/// as both only resume running again when ResumeSuspended is called from another
	/// fibre.
	Value HandleResumed(ResumeFibreArgs *result);

	/// The entrypoint function that gets called when the new stack has
	/// been created and we transfer control to it.
	WREN_MSVC_CALLCONV static void RunOnNewStack(void *oldStack, StartFibreArgs *args);

	static int stackSize;
	static std::vector<ObjFibre *> fibreCallStack;

	ObjFn *m_function = nullptr;
	void *m_stack = nullptr;

	State m_state = State::NOT_STARTED;

	/// If the fibre is suspended, this is the stack address to switch back to
	void *m_resumeAddress = nullptr;

	/// If the fibre is suspended, this is a libunwind context from the last
	/// function in that fibre's stack. This allows the GC to walk the stack
	/// of suspended fibres.
	///
	/// This is a void pointer instead of a unw_context_t to avoid importing it.
	/* unw_context_t */ void *m_suspendedContext = nullptr;

	// The GC needs to access the thread stack.
	friend GCTracingScanner;
};
