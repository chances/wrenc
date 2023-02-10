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

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

class WrenRuntime;
class CoreClasses;

/// Basically this forms a hashmap for all the function signatures.
class FunctionTable {
  public:
	struct Entry {
		void *func = nullptr;
		SignatureId signature = {};
	};

	using StlSigMap = std::unordered_map<uint64_t /*SignatureId*/, Entry>;

	/// 256 entries sounds like a reasonable number, though we might need to add a way to tweak it later.
	/// Collisions should be reasonably unlikely, though as the number of entries goes above maybe 200 the lookup
	/// times will start to get extremely long.
	/// The way this works is that given a signatureId, to find it's corresponding entry or prove it doesn't exist:
	/// 1. Lookup the entry at index signatureId % size(entries), call it entry
	/// 2. Check if entry->func is null, if so the function doesn't exist
	/// 3. Check if entry->signatureId == signatureId, if so we've found the entry
	/// 4. Advance to the next entry in the array (wrapping around) and go to step 2.
	Entry entries[256] = {};

	/// If the entries map reached saturation (it's a fixed-size array, and some classes may have
	/// so many methods they fill it up), then use a dynamically-allocated STL map.
	/// This isn't as performant, but works efficiently for an unlimited number of methods.
	/// If this is non-empty, it MUST be used in preference to [entries].
	std::optional<StlSigMap> stlEntryMap;

	static constexpr unsigned int NUM_ENTRIES = sizeof(entries) / sizeof(entries[0]);
};

/// Instances of this represent either the top-level Class, metaclasses or
/// object classes in Wren / (see the diagram in the file header for more information)
class ObjClass : public Obj {
  public:
	virtual ~ObjClass() override;

	/// Create a new ObjClass. It's parent class is left at nullptr which is invalid, you're
	/// supposed to set that up after calling this.
	ObjClass();

	/// The method dispatch table. This is put up front to make it's offset easily predictable
	/// without knowing the length of std::string, since this will be used by the generated
	/// code for EVERY METHOD CALL (hence why it has to be inlined).
	FunctionTable functions;

	/// An iterable array of functions that are actually defined on the class
	std::vector<FunctionTable::Entry> definedFunctions;

	/// The name of the class. For metaclasses, this is the same as the class's name, but
	/// the [isMetaClass] flag is set to indicate this.
	std::string name;

	/// True if this class either defines a metaclass or the root 'Class' class.
	bool isMetaClass = false;

	/// The object this class extends from. This is represented as single arrows in the
	/// diagram in the file header. This is only nullptr for the 'Object' class.
	/// For metaclasses, this always points to the root 'Class' class.
	ObjClass *parentClass = nullptr;

	/// Can this object be subclassed by a Wren object? This can only be safely set to true if
	/// the C++ class doesn't have any custom fields, as the class will actually be stored in
	/// memory as a ObjManaged instance, and wrongly casted to this type when a super method
	/// is called.
	/// Defaults to false, since this is quite a specialised thing to do.
	/// Note that Obj can be safely subclassed, but this returns false by default to protect
	/// any other classes from accidentally allowing this.
	virtual bool CanScriptSubclass();

	/// Does this class inherit methods from it's parent class? Defaults to true, used by Num which
	/// has it's own special members because it's generally just special.
	virtual bool InheritsMethods();

	/// Check if this class extends another class. This is similar to the 'is' operator:
	/// "a is b" is the same as "a.type.Extends(b)"
	bool Extends(ObjClass *other);

	/// Find a virtual function in the virtual function table. Returns nullptr if it's not found.
	FunctionTable::Entry *LookupMethod(SignatureId signature);

	/// Add a function to this class. This should only be used for initialising new classes.
	void AddFunction(const std::string &signature, void *funcPtr, bool shouldOverride = true);

	/// See [hash_util::findSignatureId].
	/// This also registers the signature into our hash-to-name lookup table
	static SignatureId FindSignatureId(const std::string &name);

	/// Given a signature ID, find the name it's associated with.
	/// If allowUnknown is set, then the value from this should be used ONLY for diagnostics, because
	/// if we've never seen the / signature before and don't know what it's name is, it'll
	/// be returned as a hex value. With allowUnknown not set, we'll crash if we don't know what it is.
	static std::string LookupSignatureFromId(SignatureId id, bool allowUnknown);

	void MarkGCValues(GCMarkOps *ops) override;

	// Wren functions:
	WREN_METHOD(getter) std::string Name() const;
	WREN_METHOD(getter) ObjClass *Supertype() const;
	WREN_METHOD(getter) std::string ToString() const;
	/// Gets the result for calling 'attributes' on this class. Defaults to null, ObjManaged can override this.
	WREN_METHOD(getter) virtual Value Attributes();

  protected:
	/// Get a pointer to an entry in the function table. If this entry doesn't exist, the pointer to the
	/// next unoccupied slot is returned.
	FunctionTable::Entry *FindFunctionTableEntry(SignatureId signature);

	/// If we run out of space in the main signatures table, move it all to an STL unordered_map.
	void RelocateSignaturesToSTL();

	/// Bind all the auto-generated method adapters to this class. Call it with
	/// the name of your class. Calling it multiple times with multiple names is
	/// valid as part of an inheritance tree.
	/// The implementation of this function itself is auto-generated.
	static void Bind(ObjClass *cls, const std::string &type, bool isMetaClass);

	/// Get the name the metaclass should be. This is intended for use when setting up the metaclass, not
	/// for looking it up later or anything like that.
	std::string GetDefaultMetaclassName();

	// CoreClasses needs to be able to bind methods
	friend CoreClasses;
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
	ObjNativeClass(const std::string &name, const std::string &bindingName);

  private:
	ObjClass m_defaultMetaClass;

	static void FinaliseSetup(); // Called by WrenRuntime once the wren_core setup code has run
	friend WrenRuntime;
};
