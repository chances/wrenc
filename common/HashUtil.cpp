//
// Created by znix on 28/07/22.
//

#include "HashUtil.h"

#include <algorithm>

SignatureId hash_util::findSignatureId(const std::string &name) {
	static const uint64_t SIG_SEED = hashString("signature id", 0);
	uint64_t value = hashString(name, SIG_SEED);
	return SignatureId{value};
}

// Roughly use MurmurHash3 - https://github.com/aappleby/smhasher/wiki/MurmurHash3
// Source https://github.com/aappleby/smhasher/blob/61a0530f282/src/MurmurHash3.cpp
// Original file under 'public domain'.
// We're not using it exactly, I've made a few modifications to simplify things

static inline uint64_t finalMix64(uint64_t k) {
	k ^= k >> 33;
	k *= 0xff51afd7ed558ccd;
	k ^= k >> 33;
	k *= 0xc4ceb9fe1a85ec53;
	k ^= k >> 33;

	return k;
}

static inline uint64_t rotl64(uint64_t x, int8_t r) { return (x << r) | (x >> (64 - r)); }

uint64_t hash_util::hashData(const uint8_t *data, int len, uint64_t seed) {
	int remaining = len;

	uint64_t h1 = seed;
	uint64_t h2 = seed;

	const uint64_t C1 = 0x87c37b91114253d5;
	const uint64_t C2 = 0x4cf5ad432745937f;

	//----------
	// body

	const uint64_t *blocks = (const uint64_t *)(data);

	int i = 0;
	while (remaining >= 16) {
		uint64_t k1 = blocks[i++];
		uint64_t k2 = blocks[i++];
		remaining -= 16;

		k1 *= C1;
		k1 = rotl64(k1, 31);
		k1 *= C2;
		h1 ^= k1;

		h1 = rotl64(h1, 27);
		h1 += h2;
		h1 = h1 * 5 + 0x52dce729;

		k2 *= C2;
		k2 = rotl64(k2, 33);
		k2 *= C1;
		h2 ^= k2;

		h2 = rotl64(h2, 31);
		h2 += h1;
		h2 = h2 * 5 + 0x38495ab5;
	}

	//----------
	// tail
	// I've simplified this to process a single block padded out with zeros

	uint64_t tail[2] = {0, 0};
	std::copy(data + len - remaining, data + len, (char *)tail);

	uint64_t k1 = tail[0];
	uint64_t k2 = tail[1];

	k2 *= C2;
	k2 = rotl64(k2, 33);
	k2 *= C1;
	h2 ^= k2;

	k1 *= C1;
	k1 = rotl64(k1, 31);
	k1 *= C2;
	h1 ^= k1;

	//----------
	// finalization

	h1 ^= len;
	h2 ^= len;

	h1 += h2;
	h2 += h1;

	h1 = finalMix64(h1);
	h2 = finalMix64(h2);

	h1 += h2;
	h2 += h1;

	// We only want a 64-bit output, so just take one. They've been mixed together thoroughly, and xor-ing them
	// at this point might reduce randomness.
	(void)h2; // Avoid unused value warning

	return h1;
}

uint64_t hash_util::hashString(const std::string &value, uint64_t seed) {
	return hashData((const uint8_t *)value.data(), value.size(), seed);
}
