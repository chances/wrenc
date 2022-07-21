//
// Created by znix on 21/07/22.
//

#include "ObjSystem.h"
#include "CoreClasses.h"

// Metaclass for ObjSystem
class ObjSystemMeta : public ObjNativeClass {
  public:
	ObjSystemMeta() {
		name = "System";
		isMetaClass = true;
		parentClass = type = &CoreClasses::Instance()->RootClass();
	}
};

ObjSystem::ObjSystem() {
	static ObjSystemMeta meta;

	parentClass = &CoreClasses::Instance()->Object();
	name = meta.name;
	type = &meta;
}
