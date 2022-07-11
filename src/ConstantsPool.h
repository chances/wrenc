//
// Created by znix on 10/07/2022.
//

#pragma once

#include <string>
#include <unordered_map>
#include <vector>

class ConstantsPool {
  public:
	/**
	 * A handle to a value in the constants pool. This handle has a 'none' value, and
	 * can be used as a boolean to check this.
	 */
	struct Handle {
	  public:
		inline operator bool() const { return m_id != 0; }

		inline int GetId() const { return m_id; }

	  private:
		Handle();
		Handle(int id);

		int m_id = 0;

		friend ConstantsPool;
	};

	/**
	 * Looks up a type in the constant pool, and returns the handle of the value.
	 *
	 * If [install] is true, then if the value is not found in the constants pool, it is added and
	 * the handle to the new value is returned.
	 */
	Handle Lookup(const std::string &value, bool install = true);

  private:
	std::vector<std::string> m_strings;
	std::unordered_map<std::string, int> m_string_ids;
};
