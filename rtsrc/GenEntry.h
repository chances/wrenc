//
// Created by znix on 21/07/22.
//

#pragma once

#include "common.h"

// Also used by ObjFn
struct UpvalueStorage {
	int32_t referenceCount = 0;
	int32_t storageSize = 0; // Pretty much just for debugging
	Value storage[];

	/// Decrease this object's reference count, and free it if that hits zero.
	void Unref();
};
