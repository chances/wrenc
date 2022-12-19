//
// Created by znix on 19/12/22.
//

#pragma once

#include "common.h"

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

/// Represents the group/name combination used to access an attribute
struct AttrKey {
	/// The name of the group, or an empty string if this attribute isn't in a group
	std::string group;

	std::string name;

	struct HashFunc {
		// https://en.cppreference.com/w/cpp/utility/hash
		std::size_t operator()(const AttrKey &key) const noexcept {
			std::size_t groupHash = std::hash<std::string>{}(key.group);
			std::size_t nameHash = std::hash<std::string>{}(key.name);
			return groupHash ^ nameHash;
		}
	};
	struct EqFunc {
		bool operator()(const AttrKey &a, const AttrKey &b) const noexcept {
			return a.group == b.group && a.name == b.name;
		}
	};
};

struct AttrContent {
	/// True if this attribute is available at runtime
	bool runtimeAccess = false;

	/// If this value is a string, this contains it.
	std::optional<std::string> str;

	/// If this value is a boolean, this contains it
	std::optional<bool> boolean;

	/// If str and boolean are empty, this is the value - either a number or null.
	Value value = NULL_VAL;
};

class AttributePack {
  public:
	~AttributePack();

	std::unordered_map<AttrKey, std::vector<AttrContent>, AttrKey::HashFunc, AttrKey::EqFunc> attributes;
};
