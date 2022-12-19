//
// Created by znix on 09/12/22.
//

#include "Errors.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

using namespace errors;

[[noreturn]] void errors::wrenAbort(const char *format, ...) {
	char buffer[256];
	va_list args;
	va_start(args, format);
	vsnprintf(buffer, sizeof(buffer), format, args);
	va_end(args);

	// TODO print a stack trace
	fprintf(stderr, "%s\n", buffer);
	abort();
}

double errors::validateNum(Value arg, const char *argName) {
	if (is_value_float(arg))
		return get_number_value(arg);
	wrenAbort("%s must be a number.", argName);
}

int errors::validateIntValue(double value, const char *argName) {
	if (trunc(value) == value)
		return value;
	wrenAbort("%s must be an integer.", argName);
}
int errors::validateInt(Value arg, const char *argName) { return validateIntValue(validateNum(arg, argName), argName); }
