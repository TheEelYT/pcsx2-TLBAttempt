// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Crypto/MagicGateCrypto.h"

#include "../../3rdparty/crypto/des/des.h"

#include <algorithm>

namespace MagicGateCrypto
{
void DesEncrypt(const Key8& key, const Block8& input, Block8* output)
{
	pcsx2::thirdparty::des::EncryptBlock(key, input, output);
}

void DesDecrypt(const Key8& key, const Block8& input, Block8* output)
{
	pcsx2::thirdparty::des::DecryptBlock(key, input, output);
}

void TwoDesEncrypt(const Key16& key, const Block8& input, Block8* output)
{
	Key8 key_1 = {};
	Key8 key_2 = {};
	std::copy_n(key.begin(), 8, key_1.begin());
	std::copy_n(key.begin() + 8, 8, key_2.begin());

	Block8 stage = {};
	DesEncrypt(key_1, input, &stage);
	DesEncrypt(key_2, stage, output);
}

void TwoDesDecrypt(const Key16& key, const Block8& input, Block8* output)
{
	Key8 key_1 = {};
	Key8 key_2 = {};
	std::copy_n(key.begin(), 8, key_1.begin());
	std::copy_n(key.begin() + 8, 8, key_2.begin());

	Block8 stage = {};
	DesDecrypt(key_2, input, &stage);
	DesDecrypt(key_1, stage, output);
}
} // namespace MagicGateCrypto
