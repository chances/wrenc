//
// A platform-independent stack-walking class.
//
// Created by Campbell on 17/02/2023.
//

#pragma once

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#else
// Only use 'local unwinding' - see https://www.nongnu.org/libunwind/man/libunwind(3).html
#define UNW_LOCAL_ONLY
#include <libunwind.h>
#endif

class StackWalker;

class StackContext {
  public:
	~StackContext();

	/// Only for use by CAPTURE_STACK_CONTEXT!
	void *CreatePlatformContext();

  private:
	void *m_platformContext = nullptr;

	friend StackWalker;
};

class StackWalker {
  public:
	~StackWalker();
	StackWalker(StackContext *context);

	/// Steps to the next frame, returning true if there are more frames.
	/// Returns false if there aren't any further frames available.
	bool Step();

	// These can be called before Step.
	void *GetInstructionPointer();
	void *GetStackPointer();

  private:
	void *m_private = nullptr;
};

// Unfortunately, if we save the stack inside of a StackContext function,
// the context won't be unwindable after it returns.
// Thus we have to use a macro to do this (an inline function would
// probably work, but this is guaranteed to.
#ifdef _WIN32
#define CAPTURE_STACK_CONTEXT(ctx) RtlCaptureContext((CONTEXT *)(ctx).CreatePlatformContext());
#else
#define CAPTURE_STACK_CONTEXT(ctx) unw_getcontext((unw_context_t *)(ctx).CreatePlatformContext());
#endif
