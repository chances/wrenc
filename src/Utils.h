//
// Created by znix on 20/12/22.
//

#pragma once

#include <string>
#include <vector>

namespace utils {

std::string buildTempFilename(std::string nameTemplate);

std::string stringFindReplace(std::string str, const std::string &from, const std::string &to);

std::vector<std::string> stringSplit(const std::string &str, const std::string &sep);

std::string stringJoin(const std::vector<std::string> &parts, const std::string &sep);

} // namespace utils
