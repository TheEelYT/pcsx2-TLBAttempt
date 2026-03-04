// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include <array>

#include "common/Pcsx2Defs.h"

namespace MagicGateCrypto
{
using Block8 = std::array<u8, 8>;
using Key8 = std::array<u8, 8>;
using Key16 = std::array<u8, 16>;

void DesEncrypt(const Key8& key, const Block8& input, Block8* output);
void DesDecrypt(const Key8& key, const Block8& input, Block8* output);

// 2DES as used by the adapted backend for PCSX2 PR #4274 behavior.
void TwoDesEncrypt(const Key16& key, const Block8& input, Block8* output);
void TwoDesDecrypt(const Key16& key, const Block8& input, Block8* output);
}
