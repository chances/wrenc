//
// Created by znix on 10/07/2022.
//

#include "ConstantsPool.h"

// Maximum number of constants of each type
const int POOL_SIZE = 0x10000;

ConstantsPool::Handle::Handle() = default;
ConstantsPool::Handle::Handle(int id) : m_id(id) {}

ConstantsPool::Handle ConstantsPool::Lookup(const std::string &value, bool install) {
	auto iter = m_string_ids.find(value);
	if (iter != m_string_ids.end())
		return iter->second;

	if (!install)
		return Handle();

	int newId = m_strings.size();
	m_strings.push_back(value);
	m_string_ids[value] = newId;
	return newId;
}
