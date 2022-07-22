//
// Created by znix on 22/07/22.
//

#include "ClassDescription.h"

#include <stdio.h>
#include <stdlib.h>

struct CommandInsn {
	ClassDescription::Command command;
	uint32_t flags;
};

struct AddMethodInsn {
	const char *name;
	void *func;
};

void ClassDescription::Parse(uint8_t *data) {
	while (true) {
		CommandInsn *cmd = (CommandInsn *)data;
		data += sizeof(*cmd);

		switch (cmd->command) {
		case Command::END:
			return;
		case Command::ADD_METHOD: {
			AddMethodInsn *method = (AddMethodInsn *)data;
			data += sizeof(*method);

			methods.emplace_back(MethodDecl{
			    .name = method->name,
			    .func = method->func,
			    .isStatic = (cmd->flags & FLAG_STATIC) != 0,
			});
			break;
		}
		default:
			fprintf(stderr, "Invalid class description command %d\n", (int)cmd->command);
			abort();
		}
	}
}
