// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include <array>

#include "common/Pcsx2Defs.h"

struct PS1MemoryCardState
{
	size_t currentByte = 2;
	u8 sectorAddrMSB = 0;
	u8 sectorAddrLSB = 0;
	u8 checksum = 0;
	u8 expectedChecksum = 0;
	std::array<u8, 128> buf = {0};
};

// A global class which contains the behavior of each memory card command.
class MemoryCardProtocol
{
private:
	PS1MemoryCardState ps1McState;
	std::array<u8, 16> authDexKey = {0x17, 0x39, 0xd3, 0xbc, 0xd0, 0x2c, 0x18, 0x07, 0x4b, 0x17, 0xf0, 0xea, 0xc4, 0x66, 0x30, 0xf9};
	std::array<u8, 16> authCexKey = {0x06, 0x46, 0x7a, 0x6c, 0x5b, 0x9b, 0x82, 0x77, 0x39, 0x0f, 0x78, 0xb7, 0xf2, 0xc6, 0xa5, 0x20};
	std::array<u8, 16>* authCurrentKey = &authDexKey;
	std::array<u8, 9> authCryptBuffer = {};
	u8 authCryptOffset = 0;
	bool authCryptReceive = false;

	bool PS1Fail();
	void The2bTerminator(size_t length);
	void ReadWriteIncrement(size_t length);
	void RecalculatePS1Addr();
	void AuthXorDataFrame();

public:
	void ResetPS1State();

	void Probe();
	void UnknownWriteDeleteEnd();
	void SetSector();
	void GetSpecs();
	void SetTerminator();
	void GetTerminator();
	void WriteData();
	void ReadData();
	u8 PS1Read(u8 data);
	u8 PS1State(u8 data);
	u8 PS1Write(u8 data);
	u8 PS1Pocketstation(u8 data);
	void ReadWriteEnd();
	void EraseBlock();
	void UnknownBoot();
	void AuthXor();
	void AuthCrypt();
	void AuthF3();
	void AuthKeySelect();
	void AuthF7();
};

extern MemoryCardProtocol g_MemoryCardProtocol;
