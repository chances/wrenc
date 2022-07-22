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

		Bind("ObjSystem", true);
	}
};

ObjSystem::ObjSystem() {
	static ObjSystemMeta meta;

	parentClass = &CoreClasses::Instance()->Object();
	name = meta.name;
	type = &meta;
}

void ObjSystem::Print(Value value) {
	if (!is_object(value))
		abort(); // TODO

	Obj *obj = (Obj *)get_object_value(value);
	std::string str = obj->ToString();
	printf("System print: %s\n", str.c_str());
}
