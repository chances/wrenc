//
// Created by znix on 22/07/22.
//

#pragma once

#include "common/common.h"

#include <memory>
#include <optional>
#include <unordered_map>
#include <utility>

class RtModule;
class GCTracingScanner;
class SlabObjectAllocator;

class WrenRuntime {
  public:
	typedef void (*FibreExitHandler)(const char *errorMessage);

	DLL_EXPORT static WrenRuntime &Instance();

	/// Do all the first-time Wren setup stuff, like initialising the standard library.
	DLL_EXPORT static void Initialise();

	template <typename T, typename... Args> T *New(Args &&...args) {
		void *mem = AllocateMem(sizeof(T), alignof(T));
		return new (mem) T(std::forward<Args>(args)...);
	}

	void *AllocateMem(int size, int alignment);
	// TODO freeing memory

	SlabObjectAllocator *GetObjectAllocator();

	DLL_EXPORT Value GetCoreGlobal(const std::string &name);

	DLL_EXPORT RtModule *GetOrInitModule(void *getGlobalsFunction);

	/// Like GetOrInitModule, but it catches fibre aborts. This is mainly to get around linkage issues.
	DLL_EXPORT RtModule *GetOrInitModuleCaught(void *getGlobalsFunction);

	/// Similar to GetOrInitModule, but won't initialise a module and works on the core module.
	RtModule *GetPreInitialisedModule(void *getGlobalsFunction);

	DLL_EXPORT void RunGC();

	RtModule *GetCoreModule();

	/// Called by ObjFibre when the last fibre exists on a non-main stack.
	void LastFibreExited(std::optional<std::string> message);

	/// Sets the handler that defines what happens when the last fibre exits on a non-main stack.
	DLL_EXPORT void SetLastFibreExitHandler(FibreExitHandler handler);

  private:
	WrenRuntime();
	~WrenRuntime();

	std::unique_ptr<RtModule> m_coreModule;
	std::unordered_map<void *, std::unique_ptr<RtModule>> m_userModules;

	std::unique_ptr<SlabObjectAllocator> m_objectAllocator;
	std::unique_ptr<GCTracingScanner> m_gcScanner;

	FibreExitHandler m_fibreExitHandler = nullptr;

	// The GC needs special access to all the loaded modules
	friend GCTracingScanner;
};
