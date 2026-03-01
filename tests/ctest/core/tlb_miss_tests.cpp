// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "pcsx2/R5900.h"
#include <cstring>
#include <gtest/gtest.h>

namespace
{
void ResetExceptionState(u32 pc, u32 status, u32 entry_hi = 0)
{
	std::memset(&cpuRegs, 0, sizeof(cpuRegs));
	cpuRegs.pc = pc;
	cpuRegs.CP0.n.Status.val = status;
	cpuRegs.CP0.n.EntryHi = entry_hi;
	cpuRegs.branch = 1;
}
} // namespace

TEST(TlbMiss, LoadMissCapturesFaultAddressAndRefillVectorState)
{
	// BEV=1, EXL=0 => bootstrap refill vector.
	ResetExceptionState(0x00100000, 0x00400000, 0x00000ABC);

	cpuTlbMissR(0x12345678, 0);

	EXPECT_EQ(cpuRegs.CP0.n.BadVAddr, 0x12345678u);
	EXPECT_EQ(cpuRegs.CP0.n.Context & 0x007FFFF0, 0x001A2B30u);
	EXPECT_EQ(cpuRegs.CP0.n.EntryHi, 0x123450BCu);
	EXPECT_EQ(cpuRegs.CP0.n.Cause & 0x7Cu, EXC_CODE_TLBL);
	EXPECT_EQ(cpuRegs.CP0.n.Cause & 0x80000000u, 0u);
	EXPECT_EQ(cpuRegs.CP0.n.EPC, 0x000FFFFCu);
	EXPECT_EQ(cpuRegs.CP0.n.Status.b.EXL, 1u);
	EXPECT_EQ(cpuRegs.pc, 0xBFC00200u);
	EXPECT_EQ(cpuRegs.branch, 0);
}

TEST(TlbMiss, StoreMissPreservesDelaySlotMetadata)
{
	ResetExceptionState(0x00200000, 0x00400000, 0x00000DEF);

	cpuTlbMissW(0x87654321, 1);

	EXPECT_EQ(cpuRegs.CP0.n.BadVAddr, 0x87654321u);
	EXPECT_EQ(cpuRegs.CP0.n.Context & 0x007FFFF0, 0x0043B2A0u);
	EXPECT_EQ(cpuRegs.CP0.n.EntryHi, 0x876540EFu);
	EXPECT_EQ(cpuRegs.CP0.n.Cause & 0x7Cu, EXC_CODE_TLBS);
	EXPECT_EQ(cpuRegs.CP0.n.Cause & 0x80000000u, 0x80000000u);
	EXPECT_EQ(cpuRegs.CP0.n.EPC, 0x001FFFF8u);
	EXPECT_EQ(cpuRegs.pc, 0xBFC00200u);
}

TEST(TlbMiss, ExistingExlRoutesToGeneralExceptionVector)
{
	// BEV=1 and EXL=1 should use +0x180 offset, not refill +0x000.
	ResetExceptionState(0x00300000, 0x00400002, 0x00000123);

	cpuTlbMissR(0x00001000, 0);

	EXPECT_EQ(cpuRegs.CP0.n.EPC, 0u);
	EXPECT_EQ(cpuRegs.pc, 0xBFC00380u);
}
