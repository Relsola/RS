#pragma once

#include <cstdint>

bool cpu_bitmanip(uint32_t instruction, uint64_t lhs, uint64_t rhs, uint64_t *result);
