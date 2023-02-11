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

struct AddFieldInsn {
	const char *name;
};

struct AddAttributeGroupInsn {
	const char *groupName;
	int32_t methodId;
	int32_t count;
	struct Item {
		const char *name;
		ClassDescription::AttrType type;
		uint32_t padding;
		Value value; // Either a value, bool or char*
	} contents[];
};

static_assert(sizeof(AddAttributeGroupInsn::Item) == 24);

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
			    .isForeign = (cmd->flags & FLAG_FOREIGN) != 0,
			});
			break;
		}
		case Command::ADD_FIELD: {
			AddFieldInsn *field = (AddFieldInsn *)data;
			data += sizeof(*field);

			fields.emplace_back(FieldDecl{
			    .name = field->name,
			});
			break;
		}
		case Command::MARK_SYSTEM_CLASS: {
			isSystemClass = true;
			break;
		}
		case Command::MARK_FOREIGN_CLASS: {
			isForeignClass = true;
			break;
		}
		case Command::ADD_ATTRIBUTE_GROUP: {
			AddAttributeGroupInsn *insn = (AddAttributeGroupInsn *)data;
			AttributeGroup group = {.method = insn->methodId, .name = insn->groupName};

			for (int i = 0; i < (int)insn->count; i++) {
				AddAttributeGroupInsn::Item *item = &insn->contents[i];

				AttrContent content = {.runtimeAccess = true};

				switch (item->type) {
				case AttrType::VALUE:
					content.value = item->value;
					break;
				case AttrType::BOOLEAN:
					content.boolean = item->value != 0;
					break;
				case AttrType::STRING:
					content.str = (const char *)item->value;
					break;
				default:
					fprintf(stderr, "Invalid class description attribute ID %d\n", (int)item->type);
					abort();
				}

				group.attributes.push_back({item->name, std::move(content)});
			}

			data += sizeof(*insn) + sizeof(AddAttributeGroupInsn::Item) * insn->count;
			attributes.push_back(std::move(group));
			break;
		}
		default:
			fprintf(stderr, "Invalid class description command %d\n", (int)cmd->command);
			abort();
		}
	}
}
