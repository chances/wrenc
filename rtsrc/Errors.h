//
// Created by znix on 09/12/22.
//

#pragma once

namespace errors {

[[noreturn]] void wrenAbort(const char *format, ...) __attribute__((format(printf, 1, 2)));

} // namespace errors
