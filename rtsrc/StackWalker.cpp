//
// Created by Campbell on 17/02/2023.
//

#include "StackWalker.h"

#include <assert.h>

#ifdef _WIN32

// ----- WINDOWS -------

struct StackWalkerWinImpl {
	CONTEXT ctx;
};

StackContext::~StackContext() { delete (CONTEXT *)m_platformContext; }

void *StackContext::CreatePlatformContext() {
	assert(m_platformContext == nullptr);
	m_platformContext = new CONTEXT{};
	return m_platformContext;
}

StackWalker::StackWalker(StackContext *context) {
	// We copy the context rather than just referencing it, as
	// Step will modify it.
	m_private = new StackWalkerWinImpl{
	    .ctx = *(CONTEXT *)context->m_platformContext,
	};
}

StackWalker::~StackWalker() { delete (StackWalkerWinImpl *)m_private; }

bool StackWalker::Step() {
	StackWalkerWinImpl *impl = (StackWalkerWinImpl *)m_private;

	DWORD64 imageBase;
	PRUNTIME_FUNCTION exCallback = RtlLookupFunctionEntry(impl->ctx.Rip, &imageBase, nullptr);

	// We can be sure unwinding won't happen from a leaf function, since
	// the context is either on another fibre (and switching goes through
	// a separate assembly function), or on the current stack and that
	// function must have a function call (since it's not on the top), which
	// means it's not a leaf function.
	// See http://www.nynaeve.net/?p=105 for more information about this.
	// Thus the only reason to see null is that we've gone into code we
	// can't unwind through.
	if (!exCallback) {
		return false;
	}

	PVOID handler;
	ULONG_PTR establisher;
	RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, impl->ctx.Rip, exCallback, &impl->ctx, &handler, &establisher,
	    nullptr);

	return true;
}

void *StackWalker::GetInstructionPointer() {
	StackWalkerWinImpl *impl = (StackWalkerWinImpl *)m_private;
	return (void *)impl->ctx.Rip;
}

void *StackWalker::GetStackPointer() {
	StackWalkerWinImpl *impl = (StackWalkerWinImpl *)m_private;
	return (void *)impl->ctx.Rsp;
}

#else

// ----- LINUX -------

struct StackWalkerLinuxImpl {
	unw_context_t *ctx = nullptr;
	unw_cursor_t cursor = {};
};

StackContext::~StackContext() { delete (unw_context_t *)m_platformContext; }

void *StackContext::CreatePlatformContext() {
	assert(m_platformContext == nullptr);
	m_platformContext = new unw_context_t{};
	return m_platformContext;
}

StackWalker::~StackWalker() { delete (StackWalkerLinuxImpl *)m_private; }

StackWalker::StackWalker(StackContext *context) {
	StackWalkerLinuxImpl *impl = new StackWalkerLinuxImpl{
	    .ctx = (unw_context_t *)context->m_platformContext,
	};
	m_private = impl;

	unw_init_local(&impl->cursor, impl->ctx);
}

bool StackWalker::Step() {
	StackWalkerLinuxImpl *impl = (StackWalkerLinuxImpl *)m_private;
	return unw_step(&impl->cursor) > 0;
}

void *StackWalker::GetInstructionPointer() {
	StackWalkerLinuxImpl *impl = (StackWalkerLinuxImpl *)m_private;
	unw_word_t ip = 0;
	unw_get_reg(&impl->cursor, UNW_REG_IP, &ip);
	return (void *)ip;
}

void *StackWalker::GetStackPointer() {
	StackWalkerLinuxImpl *impl = (StackWalkerLinuxImpl *)m_private;
	unw_word_t sp = 0;
	unw_get_reg(&impl->cursor, UNW_REG_SP, &sp);
	return (void *)sp;
}

#endif
