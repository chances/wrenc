//
// Created by znix on 24/07/22.
//

#include "ObjFn.h"
#include "ObjClass.h"

#include <optional>
#include <span>

class ObjFnClass : public ObjNativeClass {
  public:
	ObjFnClass() : ObjNativeClass("Fn", "ObjFn") {}
};

ObjFn::~ObjFn() = default;
ObjFn::ObjFn(ClosureSpec *spec, void *parentStack) : Obj(Class()), spec(spec) {
	// Take the address of the relevant values on the parent function's stack, so while the parent function is
	// still in scope we can modify the values in-place (upvaluePointers gets overwritten when the variables
	// go out of scope, to point to heap memory where the values are moved to).

	Value *valueStack = (Value *)parentStack;
	upvaluePointers.reserve(spec->upvalueOffsets.size());
	for (int stackPos : spec->upvalueOffsets) {
		upvaluePointers.push_back(&valueStack[stackPos]);
	}
}

ObjClass *ObjFn::Class() {
	static ObjFnClass cls;
	return &cls;
}

ObjFn *ObjFn::New(ObjFn *fn) { return fn; }

// We *should* be safe to pass more parameters than the function takes, but this might pose some
// portability problems down the line. It's fairly easy to do it properly though, so use the correct
// number of arguments (it's also an excuse to generate some horrible-to-read but fun-to-write template goo.
struct SpecialSpan {
	std::optional<Value> first; // If this function has upvalues, this is the upvalue pointer
	std::span<const Value> remaining;

	size_t Size() const { return remaining.size() + (first.has_value() ? 1 : 0); }
};
template <int i, typename To> struct MapArg {
	using Result = To;
};
template <size_t idx> Value listGet(const SpecialSpan &span) { return span.remaining[idx - 1]; }
template <> Value listGet<0>(const SpecialSpan &span) { return span.first.value(); }
template <size_t... arg_indices>
std::optional<Value> invokePtrSeq(std::index_sequence<arg_indices...> seq, void *func, const SpecialSpan &values) {

	// If we're the wrong size, go down the chain
	if (values.Size() != seq.size()) {
		return std::optional<Value>();
	}

	// Make a typedef for a function that takes the number of parameters given by arg_indices
	typedef Value (*func_t)(typename MapArg<arg_indices, Value>::Result...);

	func_t a = (func_t)func;

	// Call it, with listGet adding the index in it's template to the values pointer
	return a(listGet<arg_indices>(values)...);
}

template <int max_arg_count> std::optional<Value> invokePtr(void *func, const SpecialSpan &values) {
	std::optional<Value> result = invokePtrSeq(std::make_index_sequence<max_arg_count>(), func, values);
	if (result)
		return result;

	return invokePtr<max_arg_count - 1>(func, values);
}

// This line is very important, clang will happily eat all your memory infinitely recursing if you leave it out
// It stops the recursion by saying you can't have a negative number of arguments
template <> std::optional<Value> invokePtr<-1>(void *func, const SpecialSpan &values) { return std::optional<Value>(); }

Value ObjFn::Call(const std::initializer_list<Value> &values) {
	if ((int)values.size() != spec->arity) {
		fprintf(stderr, "Cannot call closure '%s' with invalid arity %d (should be %d)\n", spec->name.c_str(),
		        (int)values.size(), spec->arity);
		abort();
	}

	SpecialSpan span;
	if (spec->upvalueOffsets.empty()) {
		// No upvalue table? Put the arguments in, but if there's no arguments don't
		// try to get the first item.
		if (values.size() != 0) {
			span.first = *values.begin();
			span.remaining = std::span(values.begin() + 1, values.end());
		}
	} else {
		span.first = (Value)upvaluePointers.data();
		span.remaining = std::span(values.begin(), values.end());
	}

	std::optional<Value> result = invokePtr<16>(spec->funcPtr, span);
	if (!result) {
		fprintf(stderr, "Cannot call closure '%s': internal call failure, could not find call impl (arity %d)\n",
		        spec->name.c_str(), (int)values.size());
		abort();
	}

	return result.value();
}

struct SerialisedClosureSpec {
	void *func;
	const char *name;
	int arity, upvalueCount;
};

ClosureSpec::ClosureSpec(void *specSrc) {
	const SerialisedClosureSpec *header = (SerialisedClosureSpec *)specSrc;
	specSrc = (void *)((uint64_t)specSrc + sizeof(*header));
	funcPtr = header->func;
	name = header->name;
	arity = header->arity;

	for (int i = 0; i < header->upvalueCount; i++) {
		upvalueOffsets.push_back(*(int *)specSrc);
		specSrc = (void *)((uint64_t)specSrc + sizeof(int));
	}
}
