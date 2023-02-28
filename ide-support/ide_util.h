//
// Created by znix on 27/02/23.
//

#pragma once

// Set up the symbols to export stuff from a shared library
#ifdef _MSC_VER
#define DLL_EXPORT __declspec(dllexport)
#define MARK_PRINTF_FORMAT(fmt, first)
#else
#define DLL_EXPORT __attribute__((visibility("default")))
#define MARK_PRINTF_FORMAT(fmt, first) __attribute__((format(printf, fmt, first)))
#endif

// Call to print debugging information that shouldn't usually be enabled.
void ideDebug(const char *format, ...) MARK_PRINTF_FORMAT(1, 2);
