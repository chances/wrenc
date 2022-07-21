//
// Created by znix on 22/07/22.
//

#pragma once

#include <stdint.h>

#include <string>

uint64_t hashString(const std::string &value, uint64_t seed);
uint64_t hashData(const uint8_t *data, int len, uint64_t seed);
