//
// Created by znix on 09/12/22.
//

#pragma once

#include "common/common.h"

namespace errors {

[[noreturn]] void wrenAbort(const char *format, ...) __attribute__((format(printf, 1, 2)));

double validateNum(Value arg, const char *argName);
int validateIntValue(double value, const char *argName);
int validateInt(Value arg, const char *argName);

} // namespace errors
