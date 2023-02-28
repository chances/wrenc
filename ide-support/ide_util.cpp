//
// Created by znix on 27/02/23.
//

#include "ide_util.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void ideDebug(const char *format, ...) {
	// Check once if we're to enable debug logging.
	static int enabled = 0;
	if (enabled == 0) {
		const char *env = getenv("WREN_IDE_DEBUG");
		if (env && strcmp(env, "1") == 0) {
			enabled = 2;
		} else {
			enabled = 1;
		}
	}
	if (enabled != 2) {
		return;
	}

	char buffer[256];
	va_list args;
	va_start(args, format);
	vsnprintf(buffer, sizeof(buffer), format, args);
	va_end(args);

	printf("[ide debug] %s\n", buffer);
}
