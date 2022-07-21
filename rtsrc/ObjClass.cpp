//
// Created by znix on 21/07/22.
//

#include "ObjClass.h"
#include "hash.h"

SignatureId ObjClass::FindSignatureId(const std::string &name) {
	static const uint64_t SIG_SEED = hashString("signature id", 0);
	uint64_t value = hashString(name, SIG_SEED);
	return SignatureId{value};
}
