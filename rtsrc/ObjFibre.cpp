//
// Created by znix on 28/07/22.
//

#include "ObjFibre.h"
#include "ObjClass.h"

class ObjFibreClass : public ObjNativeClass {
  public:
	ObjFibreClass() : ObjNativeClass("Fiber", "ObjFibre") {}
};

ObjFibre::~ObjFibre() {}
ObjFibre::ObjFibre() : Obj(Class()) {}

ObjClass *ObjFibre::Class() {
	static ObjFibreClass cls;
	return &cls;
}

void ObjFibre::MarkGCValues(GCMarkOps *ops) {
	// Fibres are not yet implemented.
}
