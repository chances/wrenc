//
// Created by znix on 22/07/22.
//

#include "WrenRuntime.h"
#include "GenEntry.h"
#include <malloc.h>
#include <stdint.h>
#include <stdlib.h>

extern "C" {
// Defined in wren_core
void wren_core_3a_3a__root_func();
}

WrenRuntime::WrenRuntime() {}
WrenRuntime::~WrenRuntime() {}

WrenRuntime &WrenRuntime::Instance() {
	static WrenRuntime rt;
	return rt;
}

void *WrenRuntime::AllocateMem(int size, int alignment) {
	void *mem = malloc(size);
	if ((uint64_t)mem % alignment) {
		fprintf(stderr, "Bad alignment requirement for allocation: %d for %d and got %p\n", alignment, size, mem);
		abort();
	}
	return mem;
}

void WrenRuntime::Initialise() {
	wren_core_3a_3a__root_func();
}
