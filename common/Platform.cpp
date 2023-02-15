//
// Created by Campbell on 14/02/2023.
//

#define WIN32_LEAN_AND_MEAN

#include "Platform.h"

#include <assert.h>
#include <vector>

namespace mm = mem_management;

#ifdef _WIN32

#include <Windows.h>

int mm::getPageSize() {
	SYSTEM_INFO info = {};
	GetSystemInfo(&info);
	return info.dwAllocationGranularity;
}

void *mm::allocateMemory(int size) {
	void *mem = VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
	return mem;
}

bool mem_management::allocateMemoryAtAddress(void *addr, int size, bool &outCollided) {
	outCollided = false;

	void *mem = VirtualAlloc(addr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);

	if (mem == nullptr) {
		if (GetLastError() == ERROR_INVALID_ADDRESS) {
			outCollided = true;
		}
		return false;
	}
	assert(mem == addr);

	return true;
}

bool mm::deallocateMemory(void *addr, int size) { return VirtualFree(addr, 0, MEM_RELEASE) != 0; }

std::unique_ptr<DyLib> DyLib::Load(const std::string &filename) {
	// TODO unicode
	HMODULE module = LoadLibraryA(filename.c_str());
	if (!module) {
		std::string error = plat_util::getLastWindowsError();
		fprintf(stderr, "Failed to load shared library '%s': %s\n", filename.c_str(), error.c_str());
		exit(1);
	}

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

std::string plat_util::getExeName() {
	std::vector<char> buf;
	buf.resize(MAX_PATH + 1);

	if (GetModuleFileNameA(nullptr, buf.data(), buf.size() - 1) == 0) {
		// TODO error message
		fprintf(stderr, "Failed to read executable path!\n");
		exit(1);
	}

	std::string result = buf.data();
	return result;
}

std::string plat_util::getWindowsError(int error) {
	std::vector<char> message;
	message.resize(256);
	FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, error, 0, message.data(),
	    message.size() - 1, nullptr);
	std::string result = message.data();

	// Trim off any trailing whitespace, notably a newline it might contain
	while (!result.empty() && isspace(result.back()))
		result.pop_back();

	return result;
}

std::string plat_util::getLastWindowsError() { return getWindowsError(GetLastError()); }

#else

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

std::string plat_util::getExeName() {
	std::vector<char> buf;
	buf.resize(1024);

	if (readlink("/proc/self/exe", buf.data(), buf.size() - 1) == -1) {
		fprintf(stderr, "Failed to read executable path: %d %s\n", errno, strerror(errno));
		exit(1);
	}

	std::string result = buf.data();
	return result;
}

#endif
