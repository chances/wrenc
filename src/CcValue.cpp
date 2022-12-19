//
// Created by znix on 10/07/2022.
//

#include "CcValue.h"

#include <fmt/format.h>

const CcValue CcValue::NULL_VALUE;

CcValue::~CcValue() = default;

std::string CcValue::CheckString() const {
	if (type != STRING) {
		fmt::print(stderr, "Expected token to be string, was type {}!", type);
		abort();
	}
	return s;
}
