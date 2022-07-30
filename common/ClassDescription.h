//
// Created by znix on 22/07/22.
//

#pragma once

#include <stdint.h>
#include <string>
#include <vector>

class ClassDescription {
  public:
	enum class Command {
		END = 0,
		ADD_METHOD,
		ADD_FIELD,
		MARK_SYSTEM_CLASS,
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

	void Parse(uint8_t *data);

	bool isSystemClass = false;
	std::vector<MethodDecl> methods;
	std::vector<FieldDecl> fields;
};
