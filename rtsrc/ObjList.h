//
// Created by znix on 22/07/22.
//

#pragma once

#include "ObjClass.h"

#include <vector>

class ObjListIterator;

class ObjList : public Obj {
  public:
	ObjList();
	virtual ~ObjList();

	static ObjClass *Class();

	void MarkGCValues(GCMarkOps *ops) override;

	void ValidateIndex(int index, const char *argName) const;

	WREN_METHOD() static ObjList *New();
	WREN_METHOD() static ObjList *Filled(int size, Value element);

	WREN_METHOD() void Clear();

	WREN_METHOD() Value Add(Value toAdd);
	WREN_METHOD() Value Insert(int index, Value toAdd);
	WREN_METHOD() Value Remove(Value toRemove);
	WREN_METHOD() Value RemoveAt(int index);

	WREN_METHOD() int IndexOf(Value toFind);

	WREN_METHOD() Value Iterate(Value iterator);
	WREN_METHOD() Value IteratorValue(int iterator);

	WREN_METHOD() Value OperatorSubscript(Value indexOrRange);
	WREN_METHOD() Value OperatorSubscriptSet(int subscript, Value value);

	std::vector<Value> items;
};
