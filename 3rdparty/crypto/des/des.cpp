// SPDX-FileCopyrightText: 2024 Tareq Hossain
// SPDX-License-Identifier: MIT
// Adapted for PCSX2 from an MIT-licensed DES reference implementation:
// https://github.com/tarequeh/DES

#include "3rdparty/crypto/des/des.h"

#include <array>
#include <cstddef>

namespace pcsx2::thirdparty::des
{
namespace
{
constexpr std::array<uint8_t, 64> IP = {58, 50, 42, 34, 26, 18, 10, 2, 60, 52, 44, 36, 28, 20, 12, 4,
	62, 54, 46, 38, 30, 22, 14, 6, 64, 56, 48, 40, 32, 24, 16, 8,
	57, 49, 41, 33, 25, 17, 9, 1, 59, 51, 43, 35, 27, 19, 11, 3,
	61, 53, 45, 37, 29, 21, 13, 5, 63, 55, 47, 39, 31, 23, 15, 7};
constexpr std::array<uint8_t, 64> FP = {40, 8, 48, 16, 56, 24, 64, 32, 39, 7, 47, 15, 55, 23, 63, 31,
	38, 6, 46, 14, 54, 22, 62, 30, 37, 5, 45, 13, 53, 21, 61, 29,
	36, 4, 44, 12, 52, 20, 60, 28, 35, 3, 43, 11, 51, 19, 59, 27,
	34, 2, 42, 10, 50, 18, 58, 26, 33, 1, 41, 9, 49, 17, 57, 25};
constexpr std::array<uint8_t, 48> E = {32, 1, 2, 3, 4, 5, 4, 5, 6, 7, 8, 9, 8, 9, 10, 11,
	12, 13, 12, 13, 14, 15, 16, 17, 16, 17, 18, 19, 20, 21, 20, 21,
	22, 23, 24, 25, 24, 25, 26, 27, 28, 29, 28, 29, 30, 31, 32, 1};
constexpr std::array<uint8_t, 32> P = {16, 7, 20, 21, 29, 12, 28, 17,
	1, 15, 23, 26, 5, 18, 31, 10,
	2, 8, 24, 14, 32, 27, 3, 9,
	19, 13, 30, 6, 22, 11, 4, 25};
constexpr std::array<uint8_t, 56> PC1 = {57, 49, 41, 33, 25, 17, 9,
	1, 58, 50, 42, 34, 26, 18,
	10, 2, 59, 51, 43, 35, 27,
	19, 11, 3, 60, 52, 44, 36,
	63, 55, 47, 39, 31, 23, 15,
	7, 62, 54, 46, 38, 30, 22,
	14, 6, 61, 53, 45, 37, 29,
	21, 13, 5, 28, 20, 12, 4};
constexpr std::array<uint8_t, 48> PC2 = {14, 17, 11, 24, 1, 5,
	3, 28, 15, 6, 21, 10,
	23, 19, 12, 4, 26, 8,
	16, 7, 27, 20, 13, 2,
	41, 52, 31, 37, 47, 55,
	30, 40, 51, 45, 33, 48,
	44, 49, 39, 56, 34, 53,
	46, 42, 50, 36, 29, 32};
constexpr std::array<uint8_t, 16> SHIFTS = {1, 1, 2, 2, 2, 2, 2, 2, 1, 2, 2, 2, 2, 2, 2, 1};
constexpr std::array<std::array<uint8_t, 64>, 8> S = {{{
	14, 4, 13, 1, 2, 15, 11, 8, 3, 10, 6, 12, 5, 9, 0, 7,
	0, 15, 7, 4, 14, 2, 13, 1, 10, 6, 12, 11, 9, 5, 3, 8,
	4, 1, 14, 8, 13, 6, 2, 11, 15, 12, 9, 7, 3, 10, 5, 0,
	15, 12, 8, 2, 4, 9, 1, 7, 5, 11, 3, 14, 10, 0, 6, 13}, {
	15, 1, 8, 14, 6, 11, 3, 4, 9, 7, 2, 13, 12, 0, 5, 10,
	3, 13, 4, 7, 15, 2, 8, 14, 12, 0, 1, 10, 6, 9, 11, 5,
	0, 14, 7, 11, 10, 4, 13, 1, 5, 8, 12, 6, 9, 3, 2, 15,
	13, 8, 10, 1, 3, 15, 4, 2, 11, 6, 7, 12, 0, 5, 14, 9}, {
	10, 0, 9, 14, 6, 3, 15, 5, 1, 13, 12, 7, 11, 4, 2, 8,
	13, 7, 0, 9, 3, 4, 6, 10, 2, 8, 5, 14, 12, 11, 15, 1,
	13, 6, 4, 9, 8, 15, 3, 0, 11, 1, 2, 12, 5, 10, 14, 7,
	1, 10, 13, 0, 6, 9, 8, 7, 4, 15, 14, 3, 11, 5, 2, 12}, {
	7, 13, 14, 3, 0, 6, 9, 10, 1, 2, 8, 5, 11, 12, 4, 15,
	13, 8, 11, 5, 6, 15, 0, 3, 4, 7, 2, 12, 1, 10, 14, 9,
	10, 6, 9, 0, 12, 11, 7, 13, 15, 1, 3, 14, 5, 2, 8, 4,
	3, 15, 0, 6, 10, 1, 13, 8, 9, 4, 5, 11, 12, 7, 2, 14}, {
	2, 12, 4, 1, 7, 10, 11, 6, 8, 5, 3, 15, 13, 0, 14, 9,
	14, 11, 2, 12, 4, 7, 13, 1, 5, 0, 15, 10, 3, 9, 8, 6,
	4, 2, 1, 11, 10, 13, 7, 8, 15, 9, 12, 5, 6, 3, 0, 14,
	11, 8, 12, 7, 1, 14, 2, 13, 6, 15, 0, 9, 10, 4, 5, 3}, {
	12, 1, 10, 15, 9, 2, 6, 8, 0, 13, 3, 4, 14, 7, 5, 11,
	10, 15, 4, 2, 7, 12, 9, 5, 6, 1, 13, 14, 0, 11, 3, 8,
	9, 14, 15, 5, 2, 8, 12, 3, 7, 0, 4, 10, 1, 13, 11, 6,
	4, 3, 2, 12, 9, 5, 15, 10, 11, 14, 1, 7, 6, 0, 8, 13}, {
	4, 11, 2, 14, 15, 0, 8, 13, 3, 12, 9, 7, 5, 10, 6, 1,
	13, 0, 11, 7, 4, 9, 1, 10, 14, 3, 5, 12, 2, 15, 8, 6,
	1, 4, 11, 13, 12, 3, 7, 14, 10, 15, 6, 8, 0, 5, 9, 2,
	6, 11, 13, 8, 1, 4, 10, 7, 9, 5, 0, 15, 14, 2, 3, 12}, {
	13, 2, 8, 4, 6, 15, 11, 1, 10, 9, 3, 14, 5, 0, 12, 7,
	1, 15, 13, 8, 10, 3, 7, 4, 12, 5, 6, 11, 0, 14, 9, 2,
	7, 11, 4, 1, 9, 12, 14, 2, 0, 6, 10, 13, 15, 3, 5, 8,
	2, 1, 14, 7, 4, 10, 8, 13, 15, 12, 9, 0, 3, 5, 6, 11}}};

uint64_t ReadBE64(const Block& block)
{
	uint64_t v = 0;
	for (uint8_t b : block)
		v = (v << 8) | b;
	return v;
}

void WriteBE64(uint64_t value, Block* block)
{
	for (int i = 7; i >= 0; i--)
	{
		(*block)[i] = static_cast<uint8_t>(value & 0xFF);
		value >>= 8;
	}
}

uint64_t Permute(uint64_t input, const uint8_t* table, std::size_t out_bits, std::size_t in_bits)
{
	uint64_t out = 0;
	for (std::size_t i = 0; i < out_bits; i++)
	{
		const uint8_t src = table[i];
		out <<= 1;
		out |= (input >> (in_bits - src)) & 1ULL;
	}
	return out;
}

uint32_t Feistel(uint32_t r, uint64_t k)
{
	const uint64_t expanded = Permute(r, E.data(), 48, 32) ^ k;
	uint32_t s_out = 0;
	for (int box = 0; box < 8; box++)
	{
		const uint8_t six = static_cast<uint8_t>((expanded >> (42 - 6 * box)) & 0x3F);
		const uint8_t row = static_cast<uint8_t>(((six & 0x20) >> 4) | (six & 0x01));
		const uint8_t col = static_cast<uint8_t>((six >> 1) & 0x0F);
		s_out = (s_out << 4) | S[box][row * 16 + col];
	}
	return static_cast<uint32_t>(Permute(s_out, P.data(), 32, 32));
}

std::array<uint64_t, 16> BuildSubkeys(const Key& key)
{
	std::array<uint64_t, 16> subkeys = {};
	const uint64_t k56 = Permute(ReadBE64(key), PC1.data(), 56, 64);
	uint32_t c = static_cast<uint32_t>((k56 >> 28) & 0x0FFFFFFF);
	uint32_t d = static_cast<uint32_t>(k56 & 0x0FFFFFFF);

	for (std::size_t i = 0; i < 16; i++)
	{
		c = ((c << SHIFTS[i]) | (c >> (28 - SHIFTS[i]))) & 0x0FFFFFFF;
		d = ((d << SHIFTS[i]) | (d >> (28 - SHIFTS[i]))) & 0x0FFFFFFF;
		const uint64_t cd = (static_cast<uint64_t>(c) << 28) | d;
		subkeys[i] = Permute(cd, PC2.data(), 48, 56);
	}
	return subkeys;
}

void ProcessBlock(const Key& key, const Block& input, Block* output, bool decrypt)
{
	const auto subkeys = BuildSubkeys(key);
	uint64_t ip = Permute(ReadBE64(input), IP.data(), 64, 64);
	uint32_t l = static_cast<uint32_t>(ip >> 32);
	uint32_t r = static_cast<uint32_t>(ip & 0xFFFFFFFF);

	for (std::size_t i = 0; i < 16; i++)
	{
		const uint64_t sk = subkeys[decrypt ? (15 - i) : i];
		const uint32_t next_l = r;
		const uint32_t next_r = l ^ Feistel(r, sk);
		l = next_l;
		r = next_r;
	}

	const uint64_t pre_fp = (static_cast<uint64_t>(r) << 32) | l;
	WriteBE64(Permute(pre_fp, FP.data(), 64, 64), output);
}
} // namespace

void EncryptBlock(const Key& key, const Block& input, Block* output)
{
	ProcessBlock(key, input, output, false);
}

void DecryptBlock(const Key& key, const Block& input, Block* output)
{
	ProcessBlock(key, input, output, true);
}
} // namespace pcsx2::thirdparty::des
