//
// Created by znix on 09/12/22.
//

#include "Errors.h"

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
