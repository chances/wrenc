//
// Created by znix on 10/07/2022.
//

#pragma once

#include "ArenaAllocator.h"
#include "common/common.h"

#include <memory>
#include <string>
#include <unordered_map>

class Compiler;

// The different signature syntaxes for different kinds of methods.
enum SignatureType {
	// A name followed by a (possibly empty) parenthesized parameter list. Also
	// used for binary operators.
	SIG_METHOD,

	// Just a name. Also used for unary operators.
	SIG_GETTER,

	// A name followed by "=".
	SIG_SETTER,

	// A square bracketed parameter list.
	SIG_SUBSCRIPT,

	// A square bracketed parameter list followed by "=".
	SIG_SUBSCRIPT_SETTER,

	// A constructor initializer function. This has a distinct signature to
	// prevent it from being invoked directly outside of the constructor on the
	// metaclass.
	SIG_INITIALIZER
};

class Signature {
  public:
	DLL_EXPORT ~Signature();

	std::string name;
	SignatureType type = SIG_METHOD;
	int arity = 0;

	DLL_EXPORT std::string ToString() const; // In wren_compiler.cpp

	/// Convert a string representation of a signature into the appropriate object. This doesn't handle error
	/// cases well, and is mostly for parsing hardcoded signatures from inside the compiler.
	DLL_EXPORT static Signature Parse(const std::string &stringSignature);
};

// Compilation context
class CompContext {
  public:
	DLL_EXPORT CompContext();
	DLL_EXPORT ~CompContext();

	/// The allocator to be used for compiling everything in this context
	ArenaAllocator alloc;

	Compiler *compiler = nullptr;

	// Signature deduplication - only store a single signature object for any given signature
	std::unordered_map<std::string, std::unique_ptr<Signature>> signatures;

	/// Get a signature by name, or nullptr if it doesn't already exist
	DLL_EXPORT Signature *GetSignature(const std::string &sigString);

	/// Given a signature, find the single object used to represent it
	DLL_EXPORT Signature *GetSignature(const Signature &sig);
};
