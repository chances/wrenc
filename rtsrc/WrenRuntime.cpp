//
// Created by znix on 22/07/22.
//

#include <malloc.h>
#include <stdint.h>
#include <stdlib.h>

#include "Errors.h"
#include "GCTracingScanner.h"
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

RtModule *WrenRuntime::GetOrInitModule(void *getGlobalsFunction) {
	if (m_userModules.contains(getGlobalsFunction))
		return m_userModules.at(getGlobalsFunction).get();

	typedef void *(*globalsFunc_t)();
	globalsFunc_t func = (globalsFunc_t)getGlobalsFunction;
	void *globalsTable = func();

	std::unique_ptr<RtModule> mod = std::make_unique<RtModule>(globalsTable);
	RtModule *ptr = mod.get();
	m_userModules[getGlobalsFunction] = std::move(mod);

	// Destroy the GC, which will need to rebuild it's function table with the newly-added functions in the
	// stackmap table.
	m_gcScanner.reset();

	// Run this module's initialiser function - be sure to do this AFTER inserting the module into
	// the user modules map, since this module might call GetOrInitModule again and eventually reference
	// itself, even though it's not done loading.
	typedef void (*initFunc_t)();
	initFunc_t initFunc = (initFunc_t)ptr->GetOrNull("<INTERNAL>::init_func");

	if (!initFunc) {
		errors::wrenAbort("Cannot initialise module %s: no initialiser function!", ptr->moduleName.c_str());
	}
	initFunc();

	return ptr;
}

void WrenRuntime::RunGC() {
	if (!m_gcScanner) {
		// Note that when creating the scanner, it'll dig through our list of loaded modules.
		m_gcScanner = std::make_unique<GCTracingScanner>();
	}

	m_gcScanner->BeginGCCycle();
	// TODO when we support multithreading, mark all the threads
	m_gcScanner->MarkCurrentThreadRoots();
	m_gcScanner->EndGCCycle();
}
