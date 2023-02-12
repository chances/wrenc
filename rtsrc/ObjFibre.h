//
// Created by znix on 28/07/22.
//

#pragma once

#include "Obj.h"

#include <functional>
#include <memory>
#include <optional>
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
		SUSPENDED, // This fibre called yield or transfer and is waiting to be resumed
		WAITING,   // This fibre called another fibre and is waiting for that to yield
		FINISHED,
		FAILED,
	};

	~ObjFibre();
	ObjFibre();

	static ObjClass *Class();

	static ObjFibre *GetMainThreadFibre();

	/// Runs a given function, and returns the error it throws.
	/// For limited internal use only!
	static std::optional<std::string> RunAndCatchAbort(const std::function<void()> &func);

	void MarkGCValues(GCMarkOps *ops) override;

	/// Returns true if this thread is not currently executing, due to being in either the
	/// suspended or waiting state.
	bool IsSuspended() const;

	WREN_METHOD() static ObjFibre *New(ObjFn *argument);

	WREN_METHOD() static Value Yield();
	WREN_METHOD() static Value Yield(Value argument);

	[[noreturn]] // Has to be on a different line to avoid confusing the binding generator
	WREN_METHOD() static void Abort(std::string errorMessage);

	WREN_METHOD(getter) static ObjFibre* Current();

	WREN_METHOD() Value Call();
	WREN_METHOD() Value Call(Value argument);

	WREN_METHOD() Value Try();
	WREN_METHOD() Value Try(Value argument);

	WREN_METHOD() Value Transfer();
	WREN_METHOD() Value Transfer(Value argument);

	WREN_METHOD() Value TransferError(std::string message);

	WREN_METHOD(getter) Value IsDone();
	WREN_METHOD(getter) Value Error();

  private:
	struct StartFibreArgs;
	struct ResumeFibreArgs;
	struct FibreAbortException;

	/// The main call implementation function, which both Call and Try use
	Value CallImpl(Value argument, bool isTry, bool isTransfer);

	/// If the stack isn't created, create it.
	void CheckStack();

	/// If we have a stack, delete it.
	void DeleteStack();

	/// If this is a brand-new fibre (NOT_STARTED), then create it's stack and jump to it.
	Value StartAndSwitchTo(Value argument);

	/// If this is a fibre waiting to continue (SUSPENDED), then jump back to it's stack
	/// and continue running it.
	Value ResumeSuspended(Value argument, bool terminate);

	/// Handle a fibre being resumed. Used by both StartAndSwitchTo and ResumeSuspended,
	/// as both only resume running again when ResumeSuspended is called from another
	/// fibre.
	Value HandleResumed(ResumeFibreArgs *result);

	/// The entrypoint function that gets called when the new stack has
	/// been created and we transfer control to it.
	WREN_MSVC_CALLCONV static void RunOnNewStack(void *oldStack, StartFibreArgs *args);

	static int stackSize;
	static ObjFibre *currentFibre;

	ObjFn *m_function = nullptr;
	void *m_stack = nullptr;

	State m_state = State::NOT_STARTED;

	/// If the fibre is suspended, this is the stack address to switch back to
	void *m_resumeAddress = nullptr;

	/// If this fibre is running or waiting, this is the fibre it will return
	/// control to when it exits or calls yield.
	ObjFibre *m_parent = nullptr;

	/// If the fibre failed, this is the exception that caused it.
	std::unique_ptr<FibreAbortException> m_exception;

	/// If the fibre is suspended, this is a libunwind context from the last
	/// function in that fibre's stack. This allows the GC to walk the stack
	/// of suspended fibres.
	///
	/// This is a void pointer instead of a unw_context_t to avoid importing it.
	/* unw_context_t */ void *m_suspendedContext = nullptr;

	// The GC needs to access the thread stack.
	friend GCTracingScanner;
};
