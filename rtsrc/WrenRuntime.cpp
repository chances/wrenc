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
#include "ObjFibre.h"
#include "RtModule.h"
#include "SlabObjectAllocator.h"
#include "WrenRuntime.h"

extern "C" {
// Defined in wren_core
void *wren_core_get_globals(); // NOLINT(readability-identifier-naming)
}

WrenRuntime::WrenRuntime() : m_objectAllocator(std::make_unique<SlabObjectAllocator>()) {}
WrenRuntime::~WrenRuntime() {}

WrenRuntime &WrenRuntime::Instance() {
	static WrenRuntime rt;
	return rt;
}

void *WrenRuntime::AllocateMem(int size, int alignment) {
	void *mem = malloc(size);
	if ((uint64_t)mem % alignment) {
		errors::wrenAbort("Bad alignment requirement for allocation: %d for %d and got %p", alignment, size, mem);
	}
	return mem;
}

void WrenRuntime::Initialise() {
	// Create the core module before calling the root func, as we need the module
	// to create string literals.
	Instance().m_coreModule = std::make_unique<RtModule>(wren_core_get_globals());

	// Run the core module's initialiser function. Get it's function pointer
	// from the module string table rather than just linking to it, as the
	// LLVM and QBE backends use different name mangling conventions.
	typedef void (*initFunc_t)();
	initFunc_t initFunc = (initFunc_t)Instance().m_coreModule->GetOrNull("<INTERNAL>::init_func");
	initFunc();

	ObjNativeClass::FinaliseSetup();
}

SlabObjectAllocator *WrenRuntime::GetObjectAllocator() { return m_objectAllocator.get(); }

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
	m_modulesByName[ptr->moduleName] = ptr;

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

RtModule *WrenRuntime::GetOrInitModuleCaught(void *getGlobalsFunction) {
	RtModule *mod = nullptr;

	std::optional<std::string> error;
	try {
		mod = GetOrInitModule(getGlobalsFunction);
	} catch (const ObjFibre::FibreAbortException &ex) {
		error = Obj::ToString(ex.message);
	}

	if (error) {
		fprintf(stderr, "%s\n", error->c_str());
		exit(1);
	} else {
		return mod;
	}
}

RtModule *WrenRuntime::GetPreInitialisedModule(void *getGlobalsFunction) {
	// Handle the core module as a special case
	if (getGlobalsFunction == wren_core_get_globals)
		return m_coreModule.get();

	decltype(m_userModules)::iterator iter = m_userModules.find(getGlobalsFunction);
	if (iter != m_userModules.end())
		return iter->second.get();

	errors::wrenAbort("Could not find supposedly pre-initialised module %p", getGlobalsFunction);
	return nullptr;
}

RtModule *WrenRuntime::GetModuleByName(const std::string &name) {
	auto iter = m_modulesByName.find(name);
	if (iter == m_modulesByName.end())
		return nullptr;
	return iter->second;
}

void WrenRuntime::RunGC() {
	if (!m_gcScanner) {
		// Note that when creating the scanner, it'll dig through our list of loaded modules.
		m_gcScanner = std::make_unique<GCTracingScanner>();
	}

	m_gcScanner->BeginGCCycle();

	// Mark all the module roots - this is everything from string constants to global variables.
	m_gcScanner->AddModuleRoots(m_coreModule.get());
	for (const auto &entry : m_userModules) {
		m_gcScanner->AddModuleRoots(entry.second.get());
	}

	// TODO when we support multithreading, mark all the threads
	m_gcScanner->MarkCurrentThreadRoots();

	m_gcScanner->MarkAPIRoots();

	// Walk the heap and clear out the allocator
	m_gcScanner->EndGCCycle();
}

RtModule *WrenRuntime::GetCoreModule() { return m_coreModule.get(); }

void WrenRuntime::SetLastFibreExitHandler(WrenRuntime::FibreExitHandler handler) { m_fibreExitHandler = handler; }

void WrenRuntime::LastFibreExited(std::optional<std::string> message) {
	if (m_fibreExitHandler == nullptr) {
		fprintf(stderr, "The last fibre finished on the non-main thread!\n");
		abort();
	}

	// Avoid using std::optional for ABI reasons
	const char *errMessagePtr = message ? message->c_str() : nullptr;

	m_fibreExitHandler(errMessagePtr);
	fprintf(stderr, "Last fibre exit handler returned!\n");
	abort();
}

void WrenRuntime::SetWriteHandler(WrenRuntime::WriteHandler handler) { m_writeHandler = handler; }

WrenRuntime::WriteHandler WrenRuntime::GetCurrentWriteHandler() { return m_writeHandler; }
