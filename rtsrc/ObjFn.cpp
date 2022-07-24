//
// Created by znix on 24/07/22.
//

#include "ObjFn.h"
#include "ObjClass.h"
#include <optional>

class ObjFnClass : public ObjNativeClass {
  public:
	ObjFnClass() : ObjNativeClass("Fn", "ObjFn") {}
};

ObjFn::~ObjFn() = default;
ObjFn::ObjFn(ClosureSpec *spec) : Obj(Class()), spec(spec) {}

ObjClass *ObjFn::Class() {
	static ObjFnClass cls;
	return &cls;
}

ObjFn *ObjFn::New(ObjFn *fn) { return fn; }

// We *should* be safe to pass more parameters than the function takes, but this might pose some
// portability problems down the line. It's fairly easy to do it properly though, so use the correct
// number of arguments (it's also an excuse to generate some horrible-to-read but fun-to-write template goo.
template <int i, typename To> struct MapArg { using Result = To; };
template <size_t idx> Value listGet(const Value *start) { return start[idx]; }
template <size_t... arg_indices>
std::optional<Value> invokePtrSeq(std::index_sequence<arg_indices...> seq, void *func,
                                  const std::initializer_list<Value> &values) {

	// If we're the wrong size, go down the chain
	if (values.size() != seq.size()) {
		return std::optional<Value>();
	}

	// Make a typedef for a function that takes the number of parameters given by arg_indices
	typedef Value (*func_t)(typename MapArg<arg_indices, Value>::Result...);

	func_t a = (func_t)func;

	// Call it, with listGet adding the index in it's template to the values pointer
	return a(listGet<arg_indices>(values.begin())...);
}

template <int max_arg_count> std::optional<Value> invokePtr(void *func, const std::initializer_list<Value> &values) {
	std::optional<Value> result = invokePtrSeq(std::make_index_sequence<max_arg_count>(), func, values);
	if (result)
		return result;

	return invokePtr<max_arg_count - 1>(func, values);
}

// This line is very important, clang will happily eat all your memory infinitely recursing if you leave it out
// It stops the recursion by saying you can't have a negative number of arguments
template <> std::optional<Value> invokePtr<-1>(void *func, const std::initializer_list<Value> &values) {
	return std::optional<Value>();
}

Value ObjFn::Call(const std::initializer_list<Value> &values) {
	if (values.size() != spec->arity) {
		fprintf(stderr, "Cannot call closure '%s' with invalid arity %d (should be %d)\n", spec->name.c_str(),
		        (int)values.size(), spec->arity);
		abort();
	}

	std::optional<Value> result = invokePtr<16>(spec->funcPtr, values);
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
	funcPtr = header->func;
	name = header->name;
	arity = header->arity;
}
