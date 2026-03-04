// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

struct tlbs;

extern void WriteCP0Status(u32 value);
extern void WriteCP0Config(u32 value);
extern void WriteCP0Index(u32 value);
extern void WriteCP0Random(u32 value);
extern void WriteCP0EntryLo0(u32 value);
extern void WriteCP0EntryLo1(u32 value);
extern void WriteCP0Context(u32 value);
extern void WriteCP0PageMask(u32 value);
extern void WriteCP0Wired(u32 value);
extern void WriteCP0EntryHi(u32 value);
extern void cpuUpdateOperationMode();
extern void WriteTLB(int i);
extern void UnmapTLB(const tlbs& t, int i);
extern void MapTLB(const tlbs& t, int i);

extern void COP0_UpdatePCCR();
extern void COP0_DiagnosticPCCR();
