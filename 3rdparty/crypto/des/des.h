// SPDX-FileCopyrightText: 2024 Tareq Hossain
// SPDX-License-Identifier: MIT
// Adapted for PCSX2 from an MIT-licensed DES reference implementation:
// https://github.com/tarequeh/DES

#pragma once

#include <array>
#include <cstdint>

namespace pcsx2::thirdparty::des
{
using Block = std::array<uint8_t, 8>;
using Key = std::array<uint8_t, 8>;

void EncryptBlock(const Key& key, const Block& input, Block* output);
void DecryptBlock(const Key& key, const Block& input, Block* output);
}
