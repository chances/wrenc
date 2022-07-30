//
// Created by znix on 30/07/22.
//

#include "ObjSequence.h"
#include "ObjClass.h"

class ObjSequenceClass : public ObjNativeClass {
  public:
	ObjSequenceClass() : ObjNativeClass("Sequence", "ObjSequence") {}

	// Since the layout of ObjSequence is the same as Obj, this is fine
	static_assert(sizeof(ObjSequence) == sizeof(Obj));
	bool CanScriptSubclass() override { return true; }
};

ObjClass *ObjSequence::Class() {
	static ObjSequenceClass cls;
	return &cls;
}
