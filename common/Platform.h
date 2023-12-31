//
// Cross-platform functions for memory management, dynamic
// library loading, etc.
//
// Created by Campbell on 14/02/2023.
//

#pragma once

#include <memory>
#include <string>

namespace mem_management {

int getPageSize();

void *allocateMemory(int size);

bool allocateMemoryAtAddress(void *addr, int size, bool &outCollided);

void *allocateStackMemory(int size);

bool deallocateMemory(void *addr, int size);

} // namespace mem_management

namespace plat_util {

// Find the absolute version of a filename
std::string resolveFilename(const std::string &filename);

std::string getExeName();

constexpr char PATH_SEPARATOR =
#ifdef _WIN32
    '\\'
#else
    '/'
#endif
    ;

#ifdef _WIN32
std::string getWindowsError(int error);
std::string getLastWindowsError();
#endif

} // namespace plat_util

/// Represents a dynamic-link library.
class DyLib {
  public:
	static std::unique_ptr<DyLib> Load(const std::string &filename);

	void *Lookup(const std::string &name);

  private:
	DyLib() = default;

	void *handle = nullptr;
};
