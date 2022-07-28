//
// Created by znix on 24/07/22.
//

#pragma once

#include "Obj.h"

#include <initializer_list>
#include <vector>

class ClosureSpec;

/**
 * Represents the binding of variables to a function, such that the function
 * can access local variables outside it's scope.
 *
 * The design here is largely copied from Wren, which in turn copied it from Lua. You can
 * read more about it in Bob Nystrom's great book Crafting Interpreters, which is available
 * for free online. Since he also designed Wren, it shouldn't come as a great surprise his
 * chosen method for handling upvalues is very similar to his book.
 * The chapter dealing with this: https://craftinginterpreters.com/closures.html
 *
 * (note that this class is what the book calls ObjClosure, and what the book calls ObjFn
 *  doesn't exist here, at least as an actual Obj-based object).
 *
 * The basic idea is that we have the ObjClosure class which is what Fn.new returns in Wren. This
 * is a combination of a pointer to the function data (which has the actual function pointer, the
 * function's name, the function's arity etc) and pointers to the memory in which the Value-s of
 * the local variables this function references are stored. This object is then passed as the first
 * argument to the contained function, and whenever it needs to access the parent-scoped variables
 * it'll go through the pointer contained within.
 *
 * When the closure is first constructed, these pointers point at the stack values of the variables
 * in the parent function. When the variables go out of scope in the parent function, they're copied
 * to somewhere on the heap and all the closures referencing them are modified to point to this new
 * location. This means that closures will share the variables from their parent function (that is,
 * if one modifies a variable another can read it) even when the parent goes out of scope.
 *
 * We can optimise this a bit - if the values are 'effectively final' (never modified after the closure
 * is created, and the closure doesn't modify them) then we can just copy the values, which are the
 * same size as a pointer and saves the indirection, having to fix up the pointers when the variables
 * go out of scope, and having to store the variable on the stack rather than in registers in the parent
 * function. This isn't currently implemented, though it probably wouldn't be too hard to add.
 */
class ObjFn : public Obj {
  public:
	~ObjFn();
	ObjFn(ClosureSpec *spec, void *parentStack);

	/// In Wren there's two ways to create an Fn object:
	/// * Using Fn.new { my closure }
	/// * Using something.thing { my closure }
	///
	/// In the first case an ObjFn is returned, and in the second case an ObjFn is passed as the
	/// last argument to the function. To keep things simple in the parser, just treat Fn.new like
	/// any other function, so the user passes a ObjFn as the last (and only) argument, and just
	/// return it back again.
	///
	/// The user's perfectly well allowed to create their own function that does the exact same
	/// thing, this method is merely a convenience.
	WREN_METHOD() static ObjFn *New(ObjFn *fn);

	// Variadic functions aren't a Wren thing, but with some creative code generation in gen_bindings we can get them
	WREN_METHOD(variadic) Value Call(const std::initializer_list<Value> &values);

	static ObjClass *Class();

	ClosureSpec *spec;

	// The 'next' pointer in a linked list of functions used by the compiler to keep track of what functions
	// need their upvalue pointers to be updated when the local variables they're pointing to leave the stack.
	ObjFn *upvalueFixupList = nullptr;

	// Our actual upvalues. The data of this vector gets passed in as the upvalue pack when this function is called, if
	// there's a non-zero number of upvalues (if there's no upvalues, there's no need to waste a register).
	std::vector<Value *> upvaluePointers;
};

/// Stores information about a closure, allocated on module load.
class ClosureSpec {
  public:
	ClosureSpec(void *specSrc);

	std::string name;
	int arity = -1;
	void *funcPtr = nullptr;

	// Where the upvalues are, as an index into the values array passed into the ObjFn constructor
	std::vector<int> upvalueOffsets;
};
