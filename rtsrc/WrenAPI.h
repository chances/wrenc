//
// Created by znix on 11/02/23.
//

#pragma once

#include "RtModule.h"
#include "common/ClassDescription.h"

class ObjManaged;
class ObjManagedClass;

namespace api_interface {

class ForeignClassInterface {
  public:
	ObjManaged *Allocate(ObjManagedClass *cls, Value *args, int arity);
	void Finalise(ObjManaged *obj);

	static std::unique_ptr<ForeignClassInterface> Lookup(RtModule *mod, const std::string &className);

  private:
	// Store the allocate and deallocate functions as void pointers to
	// avoid importing the public API stuff.
	void *m_allocate = nullptr;
	void *m_deallocate = nullptr;
};

void *lookupForeignMethod(RtModule *mod, const std::string &className, const ClassDescription::MethodDecl &method);

Value dispatchForeignCall(void *funcPtr, Value *args, int argsLen);

} // namespace api_interface
