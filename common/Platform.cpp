//
// Created by Campbell on 14/02/2023.
//

#define WIN32_LEAN_AND_MEAN

#include "Platform.h"

namespace mm = mem_management;

#ifdef _WIN32

#include <Windows.h>

int mm::getPageSize() {
	SYSTEM_INFO info = {};
	GetSystemInfo(&info);
	return info.dwAllocationGranularity;
}

void *mm::allocateMemory(int size) {
	void *mem = VirtualAlloc(nullptr, size, MEM_COMMIT, PAGE_READWRITE);
	return mem;
}

bool mm::deallocateMemory(void *addr, int size) { return VirtualFree(addr, 0, MEM_RELEASE) != 0; }

std::unique_ptr<DyLib> DyLib::Load(const std::string &filename) {
	// TODO unicode
	HMODULE module = LoadLibraryA(filename.c_str());
	if (!module) {
		char buf[64];
		ZeroMemory(buf, sizeof(buf));
		DWORD errId = GetLastError();
		FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS | FORMAT_MESSAGE_FROM_HMODULE,
		    nullptr, errId, 0, buf, sizeof(buf), nullptr);
		fprintf(stderr, "Failed to load shared library '%s': %s\n", filename.c_str(), buf);
		exit(1);
	}
	// TODO

	DyLib *lib = new DyLib;
	lib->handle = (void *)module;
	return std::unique_ptr<DyLib>(lib);
}

void *DyLib::Lookup(const std::string &name) {
	HMODULE module = (HMODULE)handle;
	return (void *)GetProcAddress(module, name.c_str());
}

std::string plat_util::resolveFilename(const std::string &filename) {
	int length = 1024;
	char *fullPath = (char *)malloc(length);
	if (!GetFullPathNameA(filename.c_str(), length, fullPath, nullptr)) {
		fprintf(stderr, "Failed to resolve file name '%s'.\n", filename.c_str());
		abort();
	}
	std::string result = fullPath;
	free(fullPath);
	return result;
}

#else

#include <assert.h>
#include <dlfcn.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

int mm::getPageSize() {
	int pageSize = (int)sysconf(_SC_PAGE_SIZE);
	if (pageSize == -1) {
		fprintf(stderr, "Failed to find page size in arena allocator. Error: %d %s\n", errno, strerror(errno));
		abort();
	}
	return pageSize;
}

void *mm::allocateMemory(int size) {
	void *addr = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (addr == MAP_FAILED) {
		return nullptr;
	}
	return addr;
}

bool mm::deallocateMemory(void *addr, int size) { return munmap(addr, size) == 0; }

bool mem_management::allocateMemoryAtAddress(void *addr, int size, bool &outCollided) {
	outCollided = false;

	void *mem = mmap(addr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);

	if (mem == MAP_FAILED) {
		if (errno == EEXIST) {
			outCollided = true;
		}
		return false;
	}
	assert(mem == addr);

	return true;
}

std::unique_ptr<DyLib> DyLib::Load(const std::string &filename) {
	void *handle = dlopen(filename.c_str(), RTLD_NOW);
	if (!handle) {
		fprintf(stderr, "Failed to load shared library '%s': %s\n", filename.c_str(), dlerror());
		exit(1);
	}

	DyLib *lib = new DyLib;
	lib->handle = handle;
	return std::unique_ptr<DyLib>(lib);
}

void *DyLib::Lookup(const std::string &name) { return dlsym(handle, name.c_str()); }

std::string plat_util::resolveFilename(const std::string &filename) {
	char *resolvedC = realpath(filename.c_str(), nullptr);
	if (!resolvedC) {
		fprintf(stderr, "Failed to resolve source file name '%s' - error %d %s!\n", filename.c_str(), errno,
		    strerror(errno));
		abort();
	}
	std::string result = resolvedC;
	free(resolvedC);
	return result;
}

#endif
