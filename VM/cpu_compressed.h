#pragma once

#include <cstdint>

// 成功时返回等价的整数指令。链接地址、下一条 PC 仍由原始指令长度决定。
bool cpu_expand_compressed(uint16_t encoded, uint32_t *expanded);
