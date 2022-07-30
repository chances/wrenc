//
// Created by znix on 30/07/22.
//

#include "ObjRange.h"
#include "ObjClass.h"

class ObjRangeClass : public ObjNativeClass {
  public:
	ObjRangeClass() : ObjNativeClass("Range", "ObjRange") {}
};

ObjClass *ObjRange::Class() {
	static ObjRangeClass cls;
	return &cls;
}
