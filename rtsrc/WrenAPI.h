//
// Created by znix on 11/02/23.
//

#pragma once

#include "RtModule.h"
#include "common/ClassDescription.h"

namespace api_interface {

void *lookupForeignMethod(RtModule *mod, const std::string &className, const ClassDescription::MethodDecl &method);

Value dispatchForeignCall(void *funcPtr, Value *args, int argsLen);

} // namespace api_interface
