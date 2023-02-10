//
// Created by znix on 24/07/22.
//

#include "ObjFn.h"
#include "Errors.h"
#include "GenEntry.h"
#include "ObjClass.h"

#include <optional>

class ObjFnClass : public ObjNativeClass {
  public:
	ObjFnClass() : ObjNativeClass("Fn", "ObjFn") {}
};

ObjFn::ObjFn(ClosureSpec *spec, void *parentStack, void *parentUpvaluePack) : Obj(Class()), spec(spec) {
	// WARNING: Mostly outdated comments, the native code now sets the pointers up itself.
	// Take the address of the relevant values on the parent function's stack, so while the parent function is
	// still in scope we can modify the values in-place (upvaluePointers gets overwritten when the variables
	// go out of scope, to point to heap memory where the values are moved to).
	// In the LLVM backend, we support upvalues that are based on other upvalues (which you get when you nest
	// closures) by also accepting the parent function's upvalue table, then pulling values out of that. The
	// LLVM implementation doesn't relocate it's upvalues (it always allocates them, they never live on the
	// stack) which makes this a lot simpler.

	upvaluePointers.resize(spec->upvalueOffsets.size() + spec->storageBlockCount);

	// If neither of the upvalues are passed in, the generated code will have to set up the pointers itself
	if (parentStack == nullptr && parentUpvaluePack == nullptr)
		return;

	Value *valueStack = (Value *)parentStack;
	Value **upvaluePack = (Value **)parentUpvaluePack;
	int pos = 0;
	for (int stackPos : spec->upvalueOffsets) {
		bool isDoubleUpvalue = (stackPos & (1 << 31)) != 0;
		int realValue = stackPos & 0x7fffffff;
		if (isDoubleUpvalue) {
			upvaluePointers.at(pos++) = upvaluePack[realValue];
		} else {
			upvaluePointers.at(pos++) = &valueStack[realValue];
		}
	}
}

ObjFn::~ObjFn() {
	int baseIndex = spec->upvalueOffsets.size();
	for (int i = 0; i < spec->storageBlockCount; i++) {
		UpvalueStorage *storageBlock = (UpvalueStorage *)upvaluePointers.at(i + baseIndex);
		storageBlock->Unref();
	}
}

ObjClass *ObjFn::Class() {
	static ObjFnClass cls;
	return &cls;
}

ObjFn *ObjFn::New(ObjFn *fn) { return fn; }

Value ObjFn::Call(const std::initializer_list<Value> &values) {
	if ((int)values.size() != spec->arity) {
		errors::wrenAbort("Cannot call closure '%s' with invalid arity %d (should be %d)", spec->name.c_str(),
		    (int)values.size(), spec->arity);
	}

	void *upvalueData = nullptr;
	if (!spec->upvalueOffsets.empty()) {
		upvalueData = upvaluePointers.data();
	}

	Value result = FunctionDispatch(spec->funcPtr, upvalueData, values);

	return result;
}

void ObjFn::MarkGCValues(GCMarkOps *ops) {
	// Be careful not to scan the storage block pointers
	for (int i = 0; i < (int)spec->upvalueOffsets.size(); i++) {
		ops->ReportValue(ops, *upvaluePointers.at(i));
	}

	// Don't include upvalueFixupList - it's a utility for the compiler and is never cleared.
}

struct SerialisedClosureSpec {
	void *func;
	const char *name;
	int arity, upvalueCount;
	int storageBlockCount, padding;
};

ClosureSpec::ClosureSpec(void *specSrc) {
	const SerialisedClosureSpec *header = (SerialisedClosureSpec *)specSrc;
	specSrc = (void *)((uint64_t)specSrc + sizeof(*header));
	funcPtr = header->func;
	name = header->name;
	arity = header->arity;
	storageBlockCount = header->storageBlockCount;

	for (int i = 0; i < header->upvalueCount; i++) {
		upvalueOffsets.push_back(*(int *)specSrc);
		specSrc = (void *)((uint64_t)specSrc + sizeof(int));
	}
}
