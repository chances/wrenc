//
// Created by znix on 22/07/22.
//

#pragma once

#include "AttributePack.h"

#include <map>
#include <stdint.h>
#include <string>
#include <vector>

class ClassDescription {
  public:
	enum class Command : uint32_t {
		END = 0,
		ADD_METHOD,
		ADD_FIELD,
		MARK_SYSTEM_CLASS,
		ADD_ATTRIBUTE_GROUP,
		MARK_FOREIGN_CLASS,
	};

	enum class AttrType : uint32_t {
		VALUE = 0, // Number or null
		BOOLEAN = 1,
		STRING = 2,
	};

	static constexpr uint32_t FLAG_NONE = 0;
	static constexpr uint32_t FLAG_STATIC = 1 << 0; // Indicates a function is static

	struct MethodDecl {
		std::string name;
		void *func;
		bool isStatic;
	};

	struct FieldDecl {
		std::string name;
	};

	struct AttributeGroup {
		int method;       // The method this refers to (from the methods array), or -1 for class attribtues
		std::string name; // The group name - an empty string means null
		std::vector<std::pair<std::string, AttrContent>> attributes; // Name-and-attribute pairs
	};

	void Parse(uint8_t *data);

	bool isSystemClass = false;
	bool isForeignClass = false;
	std::vector<MethodDecl> methods;
	std::vector<FieldDecl> fields;
	std::vector<AttributeGroup> attributes;
};
