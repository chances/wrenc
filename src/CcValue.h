//
// Created by znix on 10/07/2022.
//

#pragma once

#include <string>

/**
 * A representation of a value that can be represented in the compiler.
 */
class CcValue {
  public:
	enum Type {
		UNDEFINED,
		NULL_TYPE,
		STRING,
		BOOL,
		INT,
		NUM,
	};

	~CcValue();
	CcValue() : type(NULL_TYPE) {}
	CcValue(Type type) : type(type) {}
	CcValue(bool value) : type(BOOL), b(value) {}
	CcValue(std::string str) : type(STRING), s(std::move(str)) {}
	CcValue(int value) : type(INT), i(value), n(value) {}
	CcValue(double value) : type(NUM), i(value), n(value) {}

	static const CcValue NULL_VALUE;

	Type type = NULL_TYPE;

	bool b = false;
	std::string s;
	int i = 0;
	double n = 0;

	std::string CheckString() const;
};
