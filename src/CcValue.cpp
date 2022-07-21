//
// Created by znix on 10/07/2022.
//

#include "CcValue.h"

#include <fmt/format.h>

Value CcValue::ToRuntimeValue() const {
	switch (type) {
	case UNDEFINED:
		fmt::print(stderr, "Cannot convert UNDEFINED value to runtime value\n");
		abort();
		break;
	case NULL_TYPE:
		return NanSingletons::NULL_VAL;
	case STRING:
		fmt::print(stderr, "TODO string globals\n");
		abort(); // TODO
		break;
	case BOOL:
		return b ? NanSingletons::TRUE_VAL : NanSingletons::FALSE_VAL;
	case INT:
		return encode_number(i);
	case NUM:
		return encode_number(n);
	default:
		abort();
	}
}
