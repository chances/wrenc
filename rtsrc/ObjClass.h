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

/// Instances of this represent either the top-level Class, metaclasses or
/// object classes in Wren / (see the diagram in the file header for more information)
class ObjClass : public Obj {
  public:
	/// The name of the class. For metaclasses, this is the same as the class's name, but
	/// the [isMetaClass] flag is set to indicate this.
	std::string name;

	/// True if this class either defines a metaclass or the root 'Class' class.
	bool isMetaClass = false;

	/// The object this class extends from. This is represented as single arrows in the
	/// diagram in the file header. This is only nullptr for the 'Object' class.
	/// For metaclasses, this always points to the root 'Class' class.
	ObjClass *parentClass = nullptr;

	/// This class's metaclass. This is represented by double arrows in the diagram in the
	/// file header. This is only nullptr for the 'Class' class.
	/// For metaclasses, this always points to the root 'Class' class.
	ObjClass *metaClass = nullptr;
};

/// Class used to define types in C++
class ObjNativeClass : public ObjClass {};
