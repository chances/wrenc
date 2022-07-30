//
// Created by znix on 10/07/2022.
//

#include "ObjMap.h"
#include "ObjClass.h"

class ObjMapClass : public ObjNativeClass {
  public:
	ObjMapClass() : ObjNativeClass("Map", "ObjMap") {}
};

ObjClass *ObjMap::Class() {
	static ObjMapClass cls;
	return &cls;
}
