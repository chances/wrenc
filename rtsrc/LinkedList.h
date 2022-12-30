//
// Created by znix on 30/12/22.
//

#pragma once

#include <assert.h>

/// A utility class for implementing intrusive doubly-linked lists.
/// This class accesses the next and previous fields in the target
/// object using the ObjectAccess type. It has a static Prev
/// and Next method, which return T** pointers from an input
/// of T*. For example:
///
/// class ObjAcc {
///     static Obj **Next(Obj *obj) { return &obj->next; }
///     static Obj **Prev(Obj *obj) { return &obj->prev; }
/// }
template <typename T, typename ObjectAccess> class LinkedList {
  public:
	T *start = nullptr, *end = nullptr;

	void InsertAtStart(T *newObj) {
		// If this list is empty, we set both the start and end pointer to this single object.
		if (start == nullptr) {
			assert(end == nullptr && "Linked list contained null start pointer, but non-null end pointer");
			start = end = newObj;
			return;
		}

		// Link up the next and previous pointers between this object and the old first object
		*ObjectAccess::Next(newObj) = start;
		*ObjectAccess::Prev(start) = newObj;

		// And finally mark this object as the first one - there's no need to set end, since we
		// already handled the case of an empty list.
		start = newObj;
	}

	void InsertAtEnd(T *newObj) {
		// Empty list?
		if (end == nullptr) {
			assert(start == nullptr && "Linked list contained null end pointer, but a non-null start pointer");
			start = end = newObj;
			return;
		}

		// Link up the next and previous pointers with the old end object
		*ObjectAccess::Next(end) = newObj;
		*ObjectAccess::Prev(newObj) = end;

		// And finally mark this object as being at the end
		end = newObj;
	}

	void InsertAfter(T *prev, T *newObject) {
		// Find the object that comes after the new object
		T *next = *ObjectAccess::Next(prev);

		// Point the adjacent entries at our new node
		*ObjectAccess::Next(prev) = newObject;
		if (next)
			*ObjectAccess::Prev(next) = newObject;

		// Point our new node at the adjacent entries
		*ObjectAccess::Next(newObject) = next;
		*ObjectAccess::Prev(newObject) = prev;

		// Our new node can't be at the start of the list, since we're inserting it after
		// another node, but it could be at the end.
		if (end == prev)
			end = newObject;
	}

	void Remove(T *object) {
		// Grab the next and previous pointers here to save typing.
		T *next = *ObjectAccess::Next(object);
		T *prev = *ObjectAccess::Prev(object);

		// First, update the start and end pointers if our object is the first or last item (or both, in
		// which case both start and end will end up null).
		if (start == object)
			start = next;

		if (end == object)
			end = prev;

		// Update the next and previous pointers for our neighbours, if we're not at the end of the list
		if (next)
			*ObjectAccess::Prev(next) = prev;

		if (prev)
			*ObjectAccess::Next(prev) = next;

		// Zero out the next and previous pointers - this doesn't affect the actual list since this object
		// is no longer part of it, but it should avoid hard to debug issues if someone tries accessing
		// them anyway.
		*ObjectAccess::Next(object) = nullptr;
		*ObjectAccess::Prev(object) = nullptr;
	}
};
