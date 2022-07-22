//
// Created by znix on 22/07/22.
//

#pragma once

#include <utility>

class WrenRuntime {
  public:
	static WrenRuntime &Instance();

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
