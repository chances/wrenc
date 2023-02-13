//
// Created by znix on 12/02/23.
//

#pragma once

#include "pub_include/wren.h"

#include <string>

namespace wren_random {

WrenForeignClassMethods bindRandomForeignClass(const std::string &mod, const std::string &className);

WrenForeignMethodFn bindRandomForeignMethod(const std::string &mod, const std::string &className, bool isStatic,
    const std::string &signature);

} // namespace wren_random
