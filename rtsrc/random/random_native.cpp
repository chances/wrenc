//
// Created by znix on 12/02/23.
//

#include "random_native.h"

#include <random>

static void randAllocate(WrenVM *vm);
static void randFinalise(void *data);

static void randSeed0(WrenVM *vm);
static void randSeed1(WrenVM *vm);
static void randSeed16(WrenVM *vm);

static void randInt(WrenVM *vm);
static void randFloat(WrenVM *vm);

struct RandState {
	// Don't use default since we don't know it's result/seed size
	std::mt19937_64 engine;
};

WrenForeignClassMethods wren_random::bindRandomForeignClass(const std::string &mod, const std::string &className) {
	if (mod != "random" || className != "Random")
		return WrenForeignClassMethods{nullptr, nullptr};

	return WrenForeignClassMethods{
	    .allocate = randAllocate,
	    .finalize = randFinalise,
	};
}

WrenForeignMethodFn wren_random::bindRandomForeignMethod(const std::string &mod, const std::string &className,
    bool isStatic, const std::string &signature) {

	if (mod != "random")
		return nullptr;
	if (className != "Random")
		return nullptr;
	if (isStatic)
		return nullptr;

	if (signature == "seed_()")
		return randSeed0;
	if (signature == "seed_(_)")
		return randSeed1;
	if (signature == "seed_(_,_,_,_,_,_,_,_,_,_,_,_,_,_,_,_)")
		return randSeed16;

	if (signature == "int()")
		return randInt;
	if (signature == "float()")
		return randFloat;

	return nullptr;
}

static void randAllocate(WrenVM *vm) {
	// Create the object and run the constructor.
	void *mem = wrenSetSlotNewForeign(vm, 0, 0, sizeof(RandState));
	new (mem) RandState;
}

static void randFinalise(void *data) {
	RandState *state = (RandState *)data;
	state->~RandState();
}

static void randSeed0(WrenVM *vm) {
	RandState *state = (RandState *)wrenGetSlotForeign(vm, 0);

	// random_device produces a 32-bit result, double that for
	// a full 64-bit seed.
	std::random_device trueRand;
	uint64_t seed = (((uint64_t)trueRand()) << 32) + trueRand();

	state->engine.seed(seed);
}

static void randSeed1(WrenVM *vm) {
	union {
		double d;
		uint64_t u;
	} seed;
	seed.d = wrenGetSlotDouble(vm, 1);

	RandState *state = (RandState *)wrenGetSlotForeign(vm, 0);
	state->engine.seed(seed.u);
}

static void randSeed16(WrenVM *vm) {
	uint64_t seed = 0;

	// Multiply all the values together, also multiplying the previous value by
	// a prime number to try and make all the seed bytes affect the result.
	for (int i = 0; i < 16; i++) {
		union {
			double d;
			uint64_t u;
		} conv;
		conv.d = wrenGetSlotDouble(vm, 1 + i);
		seed = seed * 11 + conv.u;
	}

	RandState *state = (RandState *)wrenGetSlotForeign(vm, 0);
	state->engine.seed(seed);
}

static void randInt(WrenVM *vm) {
	RandState *state = (RandState *)wrenGetSlotForeign(vm, 0);
	uint32_t value = (uint32_t)state->engine();

	wrenSetSlotDouble(vm, 0, (double)value);
}

static void randFloat(WrenVM *vm) {
	RandState *state = (RandState *)wrenGetSlotForeign(vm, 0);
	uint64_t value = (uint64_t)state->engine();
	uint32_t upper = value >> 32;
	uint32_t lower = value & ((1 << 21) - 1);

	// Copied-and-modified from Wren's wren_opt_random.c:

	// A double has 53 bits of precision in its mantissa, and we'd like to take
	// full advantage of that, so we need 53 bits of random source data.

	// First, start with 32 random bits, shifted to the left 21 bits.
	double result = (double)upper * (1 << 21);

	// Then add another 21 random bits.
	result += (double)lower;

	// Now we have a number from 0 - (2^53). Divide be the range to get a double
	// from 0 to 1.0 (half-inclusive).
	result /= 9007199254740992.0;

	wrenSetSlotDouble(vm, 0, result);
}
