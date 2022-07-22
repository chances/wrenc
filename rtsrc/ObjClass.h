//
// Here's the class structure diagram, copied from wren_core.c:
//
// The core class diagram ends up looking like this, where single lines point
// to a class's superclass, and double lines point to its metaclass:
//
//        .------------------------------------. .====.
//        |                  .---------------. | #    #
//        v                  |               v | v    #
//   .---------.   .-------------------.   .-------.  #
//   | Object  |==>| Object metaclass  |==>| Class |=="
//   '---------'   '-------------------'   '-------'
//        ^                                 ^ ^ ^ ^
//        |                  .--------------' # | #
//        |                  |                # | #
//   .---------.   .-------------------.      # | # -.
//   |  Base   |==>|  Base metaclass   |======" | #  |
//   '---------'   '-------------------'        | #  |
//        ^                                     | #  |
//        |                  .------------------' #  | Example classes
//        |                  |                    #  |
//   .---------.   .-------------------.          #  |
//   | Derived |==>| Derived metaclass |=========="  |
//   '---------'   '-------------------'            -'
//
// Here, the classes for 'Object', 'Base' and 'Derived' in the diagram above
// are all referred to as 'object classes'.
//
// In wrenc, all object classes, metaclasses and the special "Class" class (the
// one that all the arrows lead to in the diagram above) are instances of ObjClass.
//
// Created by znix on 21/07/22.
//

#pragma once

#include "Obj.h"

#include <string>

// A macro read by the bindings generator to mark a method as being accessible from Wren.
#define WREN_METHOD()

struct SignatureId {
	uint64_t id;

	operator uint64_t() const { return id; }
};

/// Basically this forms a hashmap for all the function signatures.
class FunctionTable {
  public:
	struct Entry {
		void *func;
		SignatureId signature;
	};

	/// 256 entries sounds like a reasonable number, though we might need to add a way to tweak it later.
	/// Collisions should be reasonably unlikely, though as the number of entries goes above maybe 200 the lookup
	/// times will start to get extremely long.
	/// The way this works is that given a signatureId, to find it's corresponding entry or prove it doesn't exist:
	/// 1. Lookup the entry at index signatureId % size(entries), call it entry
	/// 2. Check if entry->func is null, if so the function doesn't exist
	/// 3. Check if entry->signatureId == signatureId, if so we've found the entry
	/// 4. Advance to the next entry in the array (wrapping around) and go to step 2.
	Entry entries[256];

	static constexpr unsigned int NUM_ENTRIES = sizeof(entries) / sizeof(entries[0]);
};

/// Instances of this represent either the top-level Class, metaclasses or
/// object classes in Wren / (see the diagram in the file header for more information)
class ObjClass : public Obj {
  public:
	/// Create a new ObjClass. It's parent class is left at nullptr which is invalid, you're
	/// supposed to set that up after calling this.
	ObjClass();

	/// The method dispatch table. This is put up front to make it's offset easily predictable
	/// without knowing the length of std::string, since this will be used by the generated
	/// code for EVERY METHOD CALL (hence why it has to be inlined).
	FunctionTable functions;

	/// The name of the class. For metaclasses, this is the same as the class's name, but
	/// the [isMetaClass] flag is set to indicate this.
	std::string name;

	/// True if this class either defines a metaclass or the root 'Class' class.
	bool isMetaClass = false;

	/// The object this class extends from. This is represented as single arrows in the
	/// diagram in the file header. This is only nullptr for the 'Object' class.
	/// For metaclasses, this always points to the root 'Class' class.
	ObjClass *parentClass = nullptr;

	/// Find a virtual function in the virtual function table. Returns nullptr if it's not found.
	FunctionTable::Entry *LookupMethod(SignatureId signature);

	/// Add a function to this class. This should only be used for initialising new classes.
	void AddFunction(const std::string &signature, void *funcPtr);

	/// Setting p=1e-6 (one-in-a-million) gives us about six million signatures. This means that
	/// in a programme with six million signatures there's about a one-in-a-million chance of a
	/// collision. And for the collision to do anything, the colliding signatures
	/// Find the hash of the signature, to get an ID to be used for identifying functions.
	/// To avoid having to build a global table of function-to-ID mappings (which would make
	/// compiling modules independently harder, or involve a special linking step) we hash the
	/// function name to get a unique ID. This means that if we get unlucky, we could get a
	/// collision where two different function signatures result in the same signature ID.
	/// The IDs are 64-bit numbers. Using the birthday paradox approximation, we can find
	/// the number of signatures we'd need to have before the probability of a collision becomes
	/// the value p:
	/// sqrt(-2 * 2^64 * log(1-p))
	/// Setting p=1e-6 (one-in-a-million) gives us about six million signatures. This means that
	/// in a programme with six million signatures there's about a one-in-a-million chance of a
	/// collision. And for the collision to do anything, the colliding signatures have to be
	/// used in a way where the runtime could confuse them. Two functions from very different
	/// objects colliding won't matter since they'll never be called on each other.
	static SignatureId FindSignatureId(const std::string &name);

	/// Given a signature ID, find the name it's associated with.
	/// The value from this should be used ONLY for diagnostics, because if we've never seen the
	/// signature before and don't know what it's name is, it'll be returned as a hex value.
	static std::string LookupSignatureFromId(SignatureId id);
};

/// Class used to define types in C++
class ObjNativeClass : public ObjClass {
  public:
	ObjNativeClass(const std::string &name, const std::string &bindingName);

  protected:
	/// Bind all the auto-generated method adapters to this class. Call it with
	/// the name of your class. Calling it multiple times with multiple names is
	/// valid as part of an inheritance tree.
	/// The implementation of this function itself is auto-generated.
	static void Bind(ObjClass *cls, const std::string &type, bool isMetaClass);

  private:
	ObjClass m_defaultMetaClass;
};
