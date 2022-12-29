//
// Created by znix on 10/07/2022.
//

#pragma once

#include "Obj.h"

#include <unordered_map>

class ObjMap : public Obj {
  public:
	static ObjClass *Class();

	ObjMap();

	void MarkGCValues(GCMarkOps *ops) override;

	WREN_METHOD() static ObjMap *New();

	WREN_METHOD() void Clear();
	WREN_METHOD() bool ContainsKey(Value key);
	WREN_METHOD(getter) int Count();
	// keys is implemented in wren_core
	WREN_METHOD() Value Remove(Value key);
	// values is implemented in wren_core
	WREN_METHOD() Value OperatorSubscript(Value key);
	WREN_METHOD() Value OperatorSubscriptSet(Value key, Value value);

	// Iterator stuff
	WREN_METHOD() Value Iterate(Value previous);
	WREN_METHOD() Value KeyIteratorValue_(Value iterator);
	WREN_METHOD() Value ValueIteratorValue_(Value iterator);

  private:
	/// Modifies a Value so it's not recognised as any special value, such as null or false.
	static Value WrapIterator(Value raw);
	static Value UnwrapIterator(Value wrapped);

	/// Validates that a value is suitable for use as a key. This should be called before anything
	/// is passed to the map, to keep the hashing/equality functions simple.
	static void ValidateKey(Value key);

	struct ValueHash {
		std::size_t operator()(Value value) const noexcept;
	};
	struct ValueEqual {
		bool operator()(Value a, Value b) const noexcept;
	};

	std::unordered_map<Value, Value, ValueHash, ValueEqual> m_contents;
};
