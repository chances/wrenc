//
// Created by znix on 10/07/2022.
//

#include "CompContext.h"

#include <assert.h>

Signature::~Signature() {}
Signature Signature::Parse(const std::string &stringSignature) {
	// Note this is a new function and is not ported over from Wren

	Signature sig;
	sig.type = SIG_GETTER;
	bool hitBrackets = false;

	// Go through and split off the arguments
	for (char c : stringSignature) {
		switch (c) {
		case '(':
			// Don't set type if we're a setter or anything like that
			if (sig.type == SIG_GETTER)
				sig.type = SIG_METHOD;
			hitBrackets = true;
			continue;
		case '[':
			sig.type = SIG_SUBSCRIPT;
			hitBrackets = true;
			continue;
		case '=':
			if (sig.type == SIG_SUBSCRIPT)
				sig.type = SIG_SUBSCRIPT_SETTER;
			else
				sig.type = SIG_SETTER;
			continue;

			// Do we need to support initialiser functions here?

			// Ignore closing brackets
		case ')':
		case ']':
			continue;
		}

		// If we're in the brackets, only count the arguments from here on.
		if (hitBrackets) {
			if (c == '_')
				sig.arity++;
			continue;
		}

		// If this is before any of the brackets or anything, then it's part of the name
		sig.name += c;
	}

	return sig;
}

CompContext::CompContext() {}
CompContext::~CompContext() = default;

Signature *CompContext::GetSignature(const std::string &sigString) {
	auto iter = signatures.find(sigString);
	if (iter != signatures.end())
		return iter->second.get();

	return nullptr;
}

Signature *CompContext::GetSignature(const Signature &sig) {
	std::string str = sig.ToString();
	Signature *ptr = GetSignature(str);

	if (!ptr) {
		signatures[str] = std::make_unique<Signature>(sig);
		ptr = GetSignature(str);
		assert(ptr);
	}

	return ptr;
}
