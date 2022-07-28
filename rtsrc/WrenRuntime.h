//
// Created by znix on 22/07/22.
//

#pragma once

#include <utility>

class WrenRuntime {
  public:
	static WrenRuntime &Instance();

	/// Do all the first-time Wren setup stuff, like initialising the standard library.
	__attribute__((visibility("default"))) static void Initialise();

	template <typename T, typename... Args> T *New(Args &&...args) {
		void *mem = AllocateMem(sizeof(T), alignof(T));
		return new (mem) T(std::forward<Args>(args)...);
	}

	void *AllocateMem(int size, int alignment);
	// TODO freeing memory

  private:
	WrenRuntime();
	~WrenRuntime();
};
