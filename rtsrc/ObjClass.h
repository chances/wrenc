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

#include "HashUtil.h"
#include "Obj.h"

#include <string>
#include <vector>

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

	/// An iterable array of functions that are actually defined on the class
	std::vector<FunctionTable::Entry *> definedFunctions;

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

	/// See [hash_util::findSignatureId].
	/// This also registers the signature into our hash-to-name lookup table
	static SignatureId FindSignatureId(const std::string &name);

	/// Given a signature ID, find the name it's associated with.
	/// If allowUnknown is set, then the value from this should be used ONLY for diagnostics, because
	/// if we've never seen the / signature before and don't know what it's name is, it'll
	/// be returned as a hex value. With allowUnknown not set, we'll crash if we don't know what it is.
	static std::string LookupSignatureFromId(SignatureId id, bool allowUnknown);
};

/// Class used to define types in C++
class ObjNativeClass : public ObjClass {
  public:
	/// Constructor for ObjNativeClass. Automatically sets up a metaclass as the parent
	/// @arg name        The accessible-to-Wren name of this class
	/// @arg bindingName The name of the C++ class, used to bind methods declared with the WREN_METHOD() macro
	/// @arg parent      The class to inherit from. Defaults to nullptr, which means Obj.
	/// @arg inheritParentMethods  Whether or not to inherit methods from the parent class. Should almost always be
	///                            left at the default value of true.
	ObjNativeClass(const std::string &name, const std::string &bindingName, ObjClass *parent = nullptr,
	               bool inheritParentMethods = true);

  protected:
	/// Bind all the auto-generated method adapters to this class. Call it with
	/// the name of your class. Calling it multiple times with multiple names is
	/// valid as part of an inheritance tree.
	/// The implementation of this function itself is auto-generated.
	static void Bind(ObjClass *cls, const std::string &type, bool isMetaClass);

  private:
	ObjClass m_defaultMetaClass;
};
