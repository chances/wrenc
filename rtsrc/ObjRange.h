//
// Created by znix on 30/07/22.
//

#pragma once

#include "Obj.h"

class ObjRange : public Obj {
  public:
	static ObjClass *Class();

	ObjRange(double from, double to, bool inclusive = false);

	void MarkGCValues(GCMarkOps *ops) override;

	// Utility function for List and String, as they support range subscripts
	void ToSubscriptUtil(int length, int &outStart, int &outEnd, bool &outReverse) const;

	double from = 0;
	double to = 0;
	bool inclusive = false;

	WREN_METHOD(getter) double From() const;
	WREN_METHOD(getter) double To() const;
	WREN_METHOD(getter) double Min() const;
	WREN_METHOD(getter) double Max() const;
	WREN_METHOD(getter) bool IsInclusive() const;
	WREN_METHOD() Value Iterate(Value prev) const;
	WREN_METHOD() double IteratorValue(double value) const;
	WREN_METHOD(getter) std::string ToString() const;
};
