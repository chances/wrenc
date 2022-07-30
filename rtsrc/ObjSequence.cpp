//
// Created by znix on 30/07/22.
//

#include "ObjSequence.h"
#include "ObjClass.h"

class ObjSequenceClass : public ObjNativeClass {
  public:
	ObjSequenceClass() : ObjNativeClass("Sequence", "ObjSequence") {}
};

ObjClass *ObjSequence::Class() {
	static ObjSequenceClass cls;
	return &cls;
}
