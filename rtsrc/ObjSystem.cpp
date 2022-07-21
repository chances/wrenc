//
// Created by znix on 21/07/22.
//

#include "ObjSystem.h"
#include "CoreClasses.h"

ObjSystem::ObjSystem() {
	isMetaClass = true;
	parentClass = metaClass = &CoreClasses::Instance()->RootClass();
}
