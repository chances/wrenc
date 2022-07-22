//
// Created by znix on 10/07/2022.
//

#pragma once

#include <stdbool.h>
#include <stdint.h>

class Obj;

#define NAN_MASK 0x7ff8000000000000 // Includes the mantissa MSB to make it a quiet NaN
#define CONTENT_MASK 0x0007ffffffffffff
#define SIGN_MASK 0x8000000000000000

// Use NaN-tagged values
// Here's a comment I pinched from wren_value.h that nicely explains how this works. Our NaN tagging scheme is the
// same as regular Wren's one:
//
// An IEEE 754 double-precision float is a 64-bit value with bits laid out like:
//
// 1 Sign bit
// | 11 Exponent bits
// | |          52 Mantissa (i.e. fraction) bits
// | |          |
// S[Exponent-][Mantissa------------------------------------------]
//
// The details of how these are used to represent numbers aren't really
// relevant here as long we don't interfere with them. The important bit is NaN.
//
// An IEEE double can represent a few magical values like NaN ("not a number"),
// Infinity, and -Infinity. A NaN is any value where all exponent bits are set:
//
//  v--NaN bits
// -11111111111----------------------------------------------------
//
// Here, "-" means "doesn't matter". Any bit sequence that matches the above is
// a NaN. With all of those "-", it obvious there are a *lot* of different
// bit patterns that all mean the same thing. NaN tagging takes advantage of
// this. We'll use those available bit patterns to represent things other than
// numbers without giving up any valid numeric values.
//
// NaN values come in two flavors: "signalling" and "quiet". The former are
// intended to halt execution, while the latter just flow through arithmetic
// operations silently. We want the latter. Quiet NaNs are indicated by setting
// the highest mantissa bit:
//
//             v--Highest mantissa bit
// -[NaN      ]1---------------------------------------------------
//
// If all of the NaN bits are set, it's not a number. Otherwise, it is.
// That leaves all of the remaining bits as available for us to play with.
// In regular Wren, we'd use the sign bit to distinguish them, but here it's
// always set to zero, as null and boolean values are just pointers (to avoid
// special cases in the ultra-performance-sensitive virtual dispatch code).
//
// v--Always zero
// S[NaN      ]1---------------------------------------------------
//
// For pointers, we are left with 51 bits of mantissa to store an address.
// That's more than enough room for a 32-bit address. Even 64-bit machines
// only actually use 48 bits for addresses, so we've got plenty. We just stuff
// the address right into the mantissa.
//
// Ta-da, double precision numbers and pointers,
// all stuffed into a single 64-bit sequence. Even better, we don't have to
// do any masking or work to extract number values: they are unmodified. This
// means maths on numbers is fast.
typedef uint64_t Value;

enum class RtErrorType {
	NAN_FLOAT,
	INVALID_PTR,
};
void rt_throw_error(RtErrorType type);

inline bool is_value_float(Value value) {
	// Make sure the upper bits don't indicate a NAN. This requires it's a quiet NaN, with the MSB set and thus
	// ensures this is not infinity.
	return (value & NAN_MASK) != NAN_MASK;
}

inline bool is_object(Value value) {
	// Check that both the sign bit and NaN mask are all ones
	return (value & NAN_MASK) == NAN_MASK;
}

inline Obj *get_object_value(Value value) { return (Obj *)(value & CONTENT_MASK); }

inline double get_number_value(Value value) {
	union {
		Value v;
		double n;
	} tmp;
	tmp.v = value;
	return tmp.n;
}

inline Value encode_number(double num) {
	union {
		Value v;
		double n;
	} tmp;
	tmp.n = num;

	if (!is_value_float(tmp.v)) {
		rt_throw_error(RtErrorType::NAN_FLOAT);
	}

	return tmp.v;
}

inline Value encode_object(Obj *obj) {
	// If this pointer would be cut into by the NaN bits, fail
	if (((uint64_t)obj) & NAN_MASK) {
		rt_throw_error(RtErrorType::INVALID_PTR);
	}

	return NAN_MASK | (uint64_t)obj;
}

// Null is just nullptr encoded with our NaN masking scheme, which
// simplifies down to just the mask.
#define NULL_VAL NAN_MASK
