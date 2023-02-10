//
// Created by znix on 19/07/22.
//

#include "ClassInfo.h"

#include "common/AttributePack.h"

// Default destructors, to put the vtables in this compilation unit
MethodInfo::~MethodInfo() = default;
ClassInfo::~ClassInfo() = default;

bool ClassInfo::IsSystemClass() const { return ExprSystemVar::SYSTEM_VAR_NAMES.contains(name); }
bool ClassInfo::IsCppSystemClass() const { return ExprSystemVar::CPP_SYSTEM_VAR_NAMES.contains(name); }
