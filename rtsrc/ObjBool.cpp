//
// Created by znix on 22/07/22.
//

#include "ObjBool.h"
#include "CoreClasses.h"
#include "ObjClass.h"

class ObjBoolClass : public ObjNativeClass {
  public:
	ObjBoolClass() {
		name = "Bool";
		parentClass = &CoreClasses::Instance()->Object();
		type = &meta;

		meta.name = name;
		meta.parentClass = meta.type = &CoreClasses::Instance()->RootClass();
		meta.isMetaClass = true;
	}

	ObjClass meta;
};

ObjBool ObjBool::objTrue(true);
ObjBool ObjBool::objFalse(false);

ObjBool::ObjBool(bool value) : m_value(value) {
	static ObjBoolClass cls;
	type = &cls;
}
