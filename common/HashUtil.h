//
// Created by znix on 28/07/22.
//

#pragma once

#include <stdint.h>
#include <string>

struct SignatureId {
	uint64_t id;

	operator uint64_t() const { return id; }
};

namespace hash_util {

/// Setting p=1e-6 (one-in-a-million) gives us about six million signatures. This means that
/// in a programme with six million signatures there's about a one-in-a-million chance of a
/// collision. And for the collision to do anything, the colliding signatures
/// Find the hash of the signature, to get an ID to be used for identifying functions.
/// To avoid having to build a global table of function-to-ID mappings (which would make
/// compiling modules independently harder, or involve a special linking step) we hash the
/// function name to get a unique ID. This means that if we get unlucky, we could get a
/// collision where two different function signatures result in the same signature ID.
/// The IDs are 64-bit numbers. Using the birthday paradox approximation, we can find
/// the number of signatures we'd need to have before the probability of a collision becomes
/// the value p:
/// sqrt(-2 * 2^64 * log(1-p))
/// Setting p=1e-6 (one-in-a-million) gives us about six million signatures. This means that
/// in a programme with six million signatures there's about a one-in-a-million chance of a
/// collision. And for the collision to do anything, the colliding signatures have to be
/// used in a way where the runtime could confuse them. Two functions from very different
/// objects colliding won't matter since they'll never be called on each other.
SignatureId findSignatureId(const std::string &name);

uint64_t hashString(const std::string &value, uint64_t seed);
uint64_t hashData(const uint8_t *data, int len, uint64_t seed);

}; // namespace hash_util
