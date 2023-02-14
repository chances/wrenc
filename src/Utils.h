//
// Created by znix on 20/12/22.
//

#pragma once

#include "common/common.h"

#include <string>
#include <vector>

namespace utils {

DLL_EXPORT std::string buildTempFilename(std::string nameTemplate);

DLL_EXPORT std::string stringFindReplace(std::string str, const std::string &from, const std::string &to);

DLL_EXPORT std::vector<std::string> stringSplit(const std::string &str, const std::string &sep);

DLL_EXPORT std::string stringJoin(const std::vector<std::string> &parts, const std::string &sep);

} // namespace utils
