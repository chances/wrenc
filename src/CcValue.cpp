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
		return encode_object(nullptr);
	case STRING:
		fmt::print(stderr, "TODO string globals\n");
		abort(); // TODO
		break;
	case BOOL:
		fmt::print(stderr, "TODO bool globals\n");
		abort(); // TODO
		break;
	case INT:
		return encode_number(i);
	case NUM:
		return encode_number(n);
	default:
		abort();
	}
}
