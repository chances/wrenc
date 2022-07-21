//
// Created by znix on 10/07/2022.
//

#pragma once

// ObjClass is really special
class ObjClass;

// These other classes aren't so special, they're just declared here for convenience.
class ObjSystem;
class ObjMap;
class ObjString;

/// An object refers to basically anything accessible by Wren (maybe except for future inline classes).
class Obj {
  public:
	ObjClass *type = nullptr;
};
