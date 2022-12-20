//
// Created by znix on 22/07/22.
//

#include <malloc.h>
#include <stdint.h>
#include <stdlib.h>

#include "Errors.h"
#include "GenEntry.h"
#include "ObjClass.h"
#include "RtModule.h"
#include "WrenRuntime.h"

extern "C" {
// Defined in wren_core
void wren_core_3a_3a__root_func();
void *wren_core_get_globals();
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
		errors::wrenAbort("Bad alignment requirement for allocation: %d for %d and got %p\n", alignment, size, mem);
	}
	return mem;
}

void WrenRuntime::Initialise() {
	wren_core_3a_3a__root_func();

	Instance().m_coreModule = std::make_unique<RtModule>(wren_core_get_globals());

	ObjNativeClass::FinaliseSetup();
}

Value WrenRuntime::GetCoreGlobal(const std::string &name) {
	Value *global = m_coreModule->GetOrNull(name);
	if (global)
		return *global;

	fprintf(stderr, "Missing core global '%s' requested by rtlib\n", name.c_str());
	abort();
}
