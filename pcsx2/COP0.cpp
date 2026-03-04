// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Common.h"
#include "COP0.h"

// Updates the CPU's mode of operation (either, Kernel, Supervisor, or User modes).
// Currently the different modes are not implemented.
// Given this function is called so much, it's commented out for now. (rama)
__ri void cpuUpdateOperationMode()
{

	//u32 value = cpuRegs.CP0.n.Status.val;

	//if (value & 0x06 ||
	//	(value & 0x18) == 0) { // Kernel Mode (KSU = 0 | EXL = 1 | ERL = 1)*/
	//	memSetKernelMode();	// Kernel memory always
	//} else { // User Mode
	//	memSetUserMode();
	//}
}

void WriteCP0Status(u32 value)
{
	COP0_UpdatePCCR();
	cpuRegs.CP0.n.Status.val = value;
	cpuSetNextEventDelta(4);
}

void WriteCP0Config(u32 value)
{
	// Protect the read-only ICacheSize (IC) and DataCacheSize (DC) bits
	cpuRegs.CP0.n.Config = value & ~0xFC0;
	cpuRegs.CP0.n.Config |= 0x440;
}

void WriteCP0Index(u32 value)
{
	// Index uses 6 entry bits (0-47 valid) and probe-failure bit31; bits 6-30 are reserved.
	cpuRegs.CP0.n.Index = value & 0x8000003f;
	TLB_LOG("TLB_CP0 cyc=%u pc=%08X reg=Index write=%08X result=%08X", cpuRegs.cycle, cpuRegs.pc, value, cpuRegs.CP0.n.Index);
}

void WriteCP0Random(u32 value)
{
	// Random is a 6-bit TLB index on the EE (48 entries), upper bits are reserved.
	cpuRegs.CP0.n.Random = value & 0x3f;
	TLB_LOG("TLB_CP0 cyc=%u pc=%08X reg=Random write=%08X result=%08X", cpuRegs.cycle, cpuRegs.pc, value, cpuRegs.CP0.n.Random);
}

void WriteCP0EntryLo0(u32 value)
{
	// EntryLo0 exposes G/V/D/C/PFN plus the scratchpad S bit (bit31); bit26-30 are reserved.
	cpuRegs.CP0.n.EntryLo0 = value & 0x83ffffff;
	TLB_LOG("TLB_CP0 cyc=%u pc=%08X reg=EntryLo0 write=%08X result=%08X", cpuRegs.cycle, cpuRegs.pc, value, cpuRegs.CP0.n.EntryLo0);
}

void WriteCP0EntryLo1(u32 value)
{
	// EntryLo1 exposes G/V/D/C/PFN; bit26-31 are reserved.
	cpuRegs.CP0.n.EntryLo1 = value & 0x03ffffff;
	TLB_LOG("TLB_CP0 cyc=%u pc=%08X reg=EntryLo1 write=%08X result=%08X", cpuRegs.cycle, cpuRegs.pc, value, cpuRegs.CP0.n.EntryLo1);
}

void WriteCP0Context(u32 value)
{
	// Context keeps PTEBase in bits31-23 and BadVPN2 in bits22-4; bits3-0 are reserved.
	cpuRegs.CP0.n.Context = value & 0xffffffF0;
	TLB_LOG("TLB_CP0 cyc=%u pc=%08X reg=Context write=%08X result=%08X", cpuRegs.cycle, cpuRegs.pc, value, cpuRegs.CP0.n.Context);
}

void WriteCP0PageMask(u32 value)
{
	// PageMask uses bits24-13 (12 mask bits); all remaining bits are reserved.
	cpuRegs.CP0.n.PageMask = value & 0x01ffe000;
	TLB_LOG("TLB_CP0 cyc=%u pc=%08X reg=PageMask write=%08X result=%08X", cpuRegs.cycle, cpuRegs.pc, value, cpuRegs.CP0.n.PageMask);
}

void WriteCP0Wired(u32 value)
{
	// Wired uses 6 bits on EE (0-47 entries are addressable by TLBWI/TLBWR); upper bits are reserved.
	const u32 wired = value & 0x3f;
	cpuRegs.CP0.n.Wired = wired;

	// Random must stay within the writable range [Wired, 47] and never exceed the 48-entry TLB.
	if (wired <= 47)
	{
		if (cpuRegs.CP0.n.Random < wired || cpuRegs.CP0.n.Random > 47)
			cpuRegs.CP0.n.Random = 47;
	}
	else
	{
		// If Wired is outside implemented entries, pin Random to the top valid entry.
		cpuRegs.CP0.n.Random = 47;
	}

	TLB_LOG("TLB_CP0 cyc=%u pc=%08X reg=Wired write=%08X result=%08X random=%08X", cpuRegs.cycle, cpuRegs.pc, value, cpuRegs.CP0.n.Wired, cpuRegs.CP0.n.Random);
}

void WriteCP0EntryHi(u32 value)
{
	// EntryHi keeps VPN2 (31-13) and ASID (7-0); bits12-8 are reserved.
	cpuRegs.CP0.n.EntryHi = value & 0xffffe0ff;
	TLB_LOG("TLB_CP0 cyc=%u pc=%08X reg=EntryHi write=%08X result=%08X", cpuRegs.cycle, cpuRegs.pc, value, cpuRegs.CP0.n.EntryHi);
}

//////////////////////////////////////////////////////////////////////////////////////////
// Performance Counters Update Stuff!
//
// Note regarding updates of PERF and TIMR registers: never allow increment to be 0.
// That happens when a game loads the MFC0 twice in the same recompiled block (before the
// cpuRegs.cycles update), and can cause games to lock up since it's an unexpected result.
//
// PERF Overflow exceptions:  The exception is raised when the MSB of the Performance
// Counter Register is set.  I'm assuming the exception continues to re-raise until the
// app clears the bit manually (needs testing).
//
// PERF Events:
//  * Event 0 on PCR 0 is unused (counter disable)
//  * Event 16 is usable as a specific counter disable bit (since CTE affects both counters)
//  * Events 17-31 are reserved (act as counter disable)
//
// Most event mode aren't supported, and issue a warning and do a standard instruction
// count.  But only mode 1 (instruction counter) has been found to be used by games thus far.
//

static __fi bool PERF_ShouldCountEvent(uint evt)
{
	switch (evt)
	{
			// This is a rough table of actions for various PCR modes.  Some of these
			// can be implemented more accurately later.  Others (WBBs in particular)
			// probably cannot without some severe complications.

			// left sides are PCR0 / right sides are PCR1

		case 1: // cpu cycle counter.
		case 2: // single/dual instruction issued
		case 3: // Branch issued / Branch mispredicated
			return true;

		case 4: // BTAC/TLB miss
		case 5: // ITLB/DTLB miss
		case 6: // Data/Instruction cache miss
			return false;

		case 7: // Access to DTLB / WBB single request fail
		case 8: // Non-blocking load / WBB burst request fail
		case 9:
		case 10:
			return false;

		case 11: // CPU address bus busy / CPU data bus busy
			return false;

		case 12: // Instruction completed
		case 13: // non-delayslot instruction completed
		case 14: // COP2/COP1 instruction complete
		case 15: // Load/Store completed
			return true;
	}

	return false;
}

// Diagnostics for event modes that we just ignore for now.  Using these perf units could
// cause compat issues in some very odd/rare games, so if this msg comes up who knows,
// might save some debugging effort. :)
void COP0_DiagnosticPCCR()
{
	if (cpuRegs.PERF.n.pccr.b.Event0 >= 7 && cpuRegs.PERF.n.pccr.b.Event0 <= 10)
		Console.Warning("PERF/PCR0 Unsupported Update Event Mode = 0x%x", cpuRegs.PERF.n.pccr.b.Event0);

	if (cpuRegs.PERF.n.pccr.b.Event1 >= 7 && cpuRegs.PERF.n.pccr.b.Event1 <= 10)
		Console.Warning("PERF/PCR1 Unsupported Update Event Mode = 0x%x", cpuRegs.PERF.n.pccr.b.Event1);
}
extern int branch;
__fi void COP0_UpdatePCCR()
{
	// Counting and counter exceptions are not performed if we are currently executing a Level 2 exception (ERL)
	// or the counting function is not enabled (CTE)
	if (cpuRegs.CP0.n.Status.b.ERL || !cpuRegs.PERF.n.pccr.b.CTE)
	{
		cpuRegs.lastPERFCycle[0] = cpuRegs.cycle;
		cpuRegs.lastPERFCycle[1] = cpuRegs.lastPERFCycle[0];
		return;
	}

	// Implemented memory mode check (kernel/super/user)

	if (cpuRegs.PERF.n.pccr.val & ((1 << (cpuRegs.CP0.n.Status.b.KSU + 2)) | (cpuRegs.CP0.n.Status.b.EXL << 1)))
	{
		// ----------------------------------
		//    Update Performance Counter 0
		// ----------------------------------

		if (PERF_ShouldCountEvent(cpuRegs.PERF.n.pccr.b.Event0))
		{
			u32 incr = cpuRegs.cycle - cpuRegs.lastPERFCycle[0];
			if (incr == 0)
				incr++;

			// use prev/XOR method for one-time exceptions (but likely less correct)
			//u32 prev = cpuRegs.PERF.n.pcr0;
			cpuRegs.PERF.n.pcr0 += incr;
			//DevCon.Warning("PCR VAL %x", cpuRegs.PERF.n.pccr.val);
			//prev ^= (1UL<<31);		// XOR is fun!
			//if( (prev & cpuRegs.PERF.n.pcr0) & (1UL<<31) )
			if ((cpuRegs.PERF.n.pcr0 & 0x80000000))
			{
				// TODO: Vector to the appropriate exception here.
				// This code *should* be correct, but is untested (and other parts of the emu are
				// not prepared to handle proper Level 2 exception vectors yet)

				//branch == 1 is probably not the best way to check for the delay slot, but it beats nothing! (Refraction)
				/*	if( branch == 1 )
				{
					cpuRegs.CP0.n.ErrorEPC = cpuRegs.pc - 4;
					cpuRegs.CP0.n.Cause |= 0x40000000;
				}
				else
				{
					cpuRegs.CP0.n.ErrorEPC = cpuRegs.pc;
					cpuRegs.CP0.n.Cause &= ~0x40000000;
				}

				if( cpuRegs.CP0.n.Status.b.DEV )
				{
					// Bootstrap vector
					cpuRegs.pc = 0xbfc00280;
				}
				else
				{
					cpuRegs.pc = 0x80000080;
				}
				cpuRegs.CP0.n.Status.b.ERL = 1;
				cpuRegs.CP0.n.Cause |= 0x20000;*/
			}
		}
	}

	if (cpuRegs.PERF.n.pccr.val & ((1 << (cpuRegs.CP0.n.Status.b.KSU + 12)) | (cpuRegs.CP0.n.Status.b.EXL << 11)))
	{
		// ----------------------------------
		//    Update Performance Counter 1
		// ----------------------------------

		if (PERF_ShouldCountEvent(cpuRegs.PERF.n.pccr.b.Event1))
		{
			u32 incr = cpuRegs.cycle - cpuRegs.lastPERFCycle[1];
			if (incr == 0)
				incr++;

			cpuRegs.PERF.n.pcr1 += incr;

			if ((cpuRegs.PERF.n.pcr1 & 0x80000000))
			{
				// TODO: Vector to the appropriate exception here.
				// This code *should* be correct, but is untested (and other parts of the emu are
				// not prepared to handle proper Level 2 exception vectors yet)

				//branch == 1 is probably not the best way to check for the delay slot, but it beats nothing! (Refraction)

				/*if( branch == 1 )
				{
					cpuRegs.CP0.n.ErrorEPC = cpuRegs.pc - 4;
					cpuRegs.CP0.n.Cause |= 0x40000000;
				}
				else
				{
					cpuRegs.CP0.n.ErrorEPC = cpuRegs.pc;
					cpuRegs.CP0.n.Cause &= ~0x40000000;
				}

				if( cpuRegs.CP0.n.Status.b.DEV )
				{
					// Bootstrap vector
					cpuRegs.pc = 0xbfc00280;
				}
				else
				{
					cpuRegs.pc = 0x80000080;
				}
				cpuRegs.CP0.n.Status.b.ERL = 1;
				cpuRegs.CP0.n.Cause |= 0x20000;*/
			}
		}
	}
	cpuRegs.lastPERFCycle[0] = cpuRegs.cycle;
	cpuRegs.lastPERFCycle[1] = cpuRegs.cycle;
}

//////////////////////////////////////////////////////////////////////////////////////////
//


static void LogTLBEvent(const char* op, s32 selected, u32 page_mask, u32 entry_hi, u32 entry_lo0, u32 entry_lo1)
{
	const bool global = ((entry_lo0 & 0x1) != 0) && ((entry_lo1 & 0x1) != 0);
	const bool scratch = (entry_lo0 & 0x80000000u) != 0;
	const bool cache0 = ((entry_lo0 >> 3) & 0x7) == 0x3;
	const bool cache1 = ((entry_lo1 >> 3) & 0x7) == 0x3;
	const u32 asid = entry_hi & 0xff;

	TLB_LOG("TLB_EVT cyc=%u pc=%08X op=%s idx=%u rnd=%u wired=%u sel=%d EntryHi=%08X PageMask=%08X EntryLo0=%08X EntryLo1=%08X ASID=%02X G=%u SPR=%u C0=%u C1=%u",
		cpuRegs.cycle, cpuRegs.pc, op, cpuRegs.CP0.n.Index & 0x3f, cpuRegs.CP0.n.Random & 0x3f, cpuRegs.CP0.n.Wired & 0x3f,
		selected, entry_hi, page_mask, entry_lo0, entry_lo1, asid, global ? 1u : 0u, scratch ? 1u : 0u, cache0 ? 1u : 0u, cache1 ? 1u : 0u);
}

static void LogTLBEvent(const char* op, s32 selected)
{
	LogTLBEvent(op, selected, cpuRegs.CP0.n.PageMask, cpuRegs.CP0.n.EntryHi, cpuRegs.CP0.n.EntryLo0, cpuRegs.CP0.n.EntryLo1);
}

static void LogTLBEntryEvent(const char* op, s32 selected, const tlbs& t)
{
	LogTLBEvent(op, selected, t.PageMask.UL, t.EntryHi.UL, t.EntryLo0.UL, t.EntryLo1.UL);
}

void MapTLB(const tlbs& t, int i)
{
	u32 mask, addr;
	u32 saddr, eaddr;

	LogTLBEntryEvent("MAP", i, t);

	// According to the manual
	// 'It [SPR] must be mapped into a contiguous 16 KB of virtual address space that is
	// aligned on a 16KB boundary.Results are not guaranteed if this restriction is not followed.'
	// Assume that the game isn't doing anything less-than-ideal with the scratchpad mapping and map it directly to eeMem->Scratch.
	if (t.isSPR())
	{
		if (t.VPN2() != 0x70000000)
			Console.Warning("COP0: Mapping Scratchpad to non-default address 0x%08X", t.VPN2());

		vtlb_VMapBuffer(t.VPN2(), eeMem->Scratch, Ps2MemSize::Scratch);
	}
	else
	{
		if (t.EntryLo0.V)
		{
			mask = ((~t.Mask()) << 1) & 0xfffff;
			saddr = t.VPN2() >> 12;
			eaddr = saddr + t.Mask() + 1;

			for (addr = saddr; addr < eaddr; addr++)
			{
				if ((addr & mask) == ((t.VPN2() >> 12) & mask))
				{ //match
					memSetPageAddr(addr << 12, t.PFN0() + ((addr - saddr) << 12));
					Cpu->Clear(addr << 12, 0x400);
				}
			}
		}

		if (t.EntryLo1.V)
		{
			mask = ((~t.Mask()) << 1) & 0xfffff;
			saddr = (t.VPN2() >> 12) + t.Mask() + 1;
			eaddr = saddr + t.Mask() + 1;

			for (addr = saddr; addr < eaddr; addr++)
			{
				if ((addr & mask) == ((t.VPN2() >> 12) & mask))
				{ //match
					memSetPageAddr(addr << 12, t.PFN1() + ((addr - saddr) << 12));
					Cpu->Clear(addr << 12, 0x400);
				}
			}
		}
	}
}

__inline u32 ConvertPageMask(const u32 PageMask)
{
	const u32 mask = std::popcount(PageMask >> 13);

	pxAssertMsg(!((mask & 1) || mask > 12), "Invalid page mask for this TLB entry. EE cache doesn't know what to do here.");

	return (1 << (12 + mask)) - 1;
}

void UnmapTLB(const tlbs& t, int i)
{
	//Console.WriteLn("Clear TLB %d: %08x-> [%08x %08x] S=%d G=%d ASID=%d Mask= %03X", i,t.VPN2,t.PFN0,t.PFN1,t.S,t.G,t.ASID,t.Mask);
	u32 mask, addr;
	u32 saddr, eaddr;

	if (t.isSPR())
	{
		vtlb_VMapUnmap(t.VPN2(), 0x4000);
		return;
	}

	if (t.EntryLo0.V)
	{
		mask = ((~t.Mask()) << 1) & 0xfffff;
		saddr = t.VPN2() >> 12;
		eaddr = saddr + t.Mask() + 1;
		//	Console.WriteLn("Clear TLB: %08x ~ %08x",saddr,eaddr-1);
		for (addr = saddr; addr < eaddr; addr++)
		{
			if ((addr & mask) == ((t.VPN2() >> 12) & mask))
			{ //match
				memClearPageAddr(addr << 12);
				Cpu->Clear(addr << 12, 0x400);
			}
		}
	}

	if (t.EntryLo1.V)
	{
		mask = ((~t.Mask()) << 1) & 0xfffff;
		saddr = (t.VPN2() >> 12) + t.Mask() + 1;
		eaddr = saddr + t.Mask() + 1;
		//	Console.WriteLn("Clear TLB: %08x ~ %08x",saddr,eaddr-1);
		for (addr = saddr; addr < eaddr; addr++)
		{
			if ((addr & mask) == ((t.VPN2() >> 12) & mask))
			{ //match
				memClearPageAddr(addr << 12);
				Cpu->Clear(addr << 12, 0x400);
			}
		}
	}

	for (size_t i = 0; i < cachedTlbs.count; i++)
	{
		if (cachedTlbs.PFN0s[i] == t.PFN0() && cachedTlbs.PFN1s[i] == t.PFN1() && cachedTlbs.PageMasks[i] == ConvertPageMask(t.PageMask.UL))
		{
			for (size_t j = i; j < cachedTlbs.count - 1; j++)
			{
				cachedTlbs.CacheEnabled0[j] = cachedTlbs.CacheEnabled0[j + 1];
				cachedTlbs.CacheEnabled1[j] = cachedTlbs.CacheEnabled1[j + 1];
				cachedTlbs.PFN0s[j] = cachedTlbs.PFN0s[j + 1];
				cachedTlbs.PFN1s[j] = cachedTlbs.PFN1s[j + 1];
				cachedTlbs.PageMasks[j] = cachedTlbs.PageMasks[j + 1];
			}
			cachedTlbs.count--;
			break;
		}
	}
}

void WriteTLB(int i)
{
	tlb[i].PageMask.UL = cpuRegs.CP0.n.PageMask;
	tlb[i].EntryHi.UL = cpuRegs.CP0.n.EntryHi;
	tlb[i].EntryLo0.UL = cpuRegs.CP0.n.EntryLo0;
	tlb[i].EntryLo1.UL = cpuRegs.CP0.n.EntryLo1;

	// Setting the cache mode to reserved values is vaguely defined in the manual.
	// I found that SPR is set to cached regardless.
	// Non-SPR entries default to uncached on reserved cache modes.
	if (tlb[i].isSPR())
	{
		tlb[i].EntryLo0.C = 3;
		tlb[i].EntryLo1.C = 3;
	}
	else
	{
		if (!tlb[i].EntryLo0.isValidCacheMode())
			tlb[i].EntryLo0.C = 2;
		if (!tlb[i].EntryLo1.isValidCacheMode())
			tlb[i].EntryLo1.C = 2;
	}

	if (!tlb[i].isSPR() && ((tlb[i].EntryLo0.V && tlb[i].EntryLo0.isCached()) || (tlb[i].EntryLo1.V && tlb[i].EntryLo1.isCached())))
	{
		const size_t idx = cachedTlbs.count;
		cachedTlbs.CacheEnabled0[idx] = tlb[i].EntryLo0.isCached() ? ~0 : 0;
		cachedTlbs.CacheEnabled1[idx] = tlb[i].EntryLo1.isCached() ? ~0 : 0;
		cachedTlbs.PFN1s[idx] = tlb[i].PFN1();
		cachedTlbs.PFN0s[idx] = tlb[i].PFN0();
		cachedTlbs.PageMasks[idx] = ConvertPageMask(tlb[i].PageMask.UL);

		cachedTlbs.count++;
	}

	MapTLB(tlb[i], i);
}

namespace R5900 {
namespace Interpreter {
namespace OpcodeImpl {
namespace COP0 {

	void TLBR()
	{
		LogTLBEvent("TLBR_REQ", cpuRegs.CP0.n.Index & 0x3f);

		const u8 i = cpuRegs.CP0.n.Index & 0x3f;

		if (i > 47)
		{
			LogTLBEvent("TLBR_OOB", i);
			return;
		}

		cpuRegs.CP0.n.PageMask = tlb[i].PageMask.Mask << 13;
		cpuRegs.CP0.n.EntryHi = tlb[i].EntryHi.UL & ~((tlb[i].PageMask.Mask << 13) | 0x1f00);
		cpuRegs.CP0.n.EntryLo0 = tlb[i].EntryLo0.UL & ~(0xFC000000) & ~1;
		cpuRegs.CP0.n.EntryLo1 = tlb[i].EntryLo1.UL & ~(0x7C000000) & ~1;
		// "If both the Global bit of EntryLo0 and EntryLo1 are set to 1, the processor ignores the ASID during TLB lookup."
		// This is reflected during TLBR, where G is only set if both EntryLo0 and EntryLo1 are global.
		cpuRegs.CP0.n.EntryLo0 |= (tlb[i].EntryLo0.UL & 1) & (tlb[i].EntryLo1.UL & 1);
		cpuRegs.CP0.n.EntryLo1 |= (tlb[i].EntryLo0.UL & 1) & (tlb[i].EntryLo1.UL & 1);
		LogTLBEvent("TLBR_RES", i);
	}

	void TLBWI()
	{
		const u8 j = cpuRegs.CP0.n.Index & 0x3f;

		if (j > 47)
		{
			LogTLBEvent("TLBWI_OOB", j);
			return;
		}

		LogTLBEvent("TLBWI", j);

		UnmapTLB(tlb[j], j);
		WriteTLB(j);
	}

	void TLBWR()
	{
		const u8 j = cpuRegs.CP0.n.Random & 0x3f;

		if (j > 47)
		{
			LogTLBEvent("TLBWR_OOB", j);
			return;
		}

		LogTLBEvent("TLBWR", j);

		UnmapTLB(tlb[j], j);
		WriteTLB(j);
	}

	void TLBP()
	{
		int i;

		union
		{
			struct
			{
				u32 VPN2 : 19;
				u32 VPN2X : 2;
				u32 G : 3;
				u32 ASID : 8;
			} s;
			u32 u;
		} EntryHi32;

		EntryHi32.u = cpuRegs.CP0.n.EntryHi;

		LogTLBEvent("TLBP_REQ", -1);
		cpuRegs.CP0.n.Index = 0xFFFFFFFF;
		for (i = 0; i < 48; i++)
		{
			if (tlb[i].VPN2() == ((~tlb[i].Mask()) & (EntryHi32.s.VPN2)) && ((tlb[i].isGlobal()) || ((tlb[i].EntryHi.ASID & 0xff) == EntryHi32.s.ASID)))
			{
				cpuRegs.CP0.n.Index = i;
				break;
			}
		}
		if (cpuRegs.CP0.n.Index == 0xFFFFFFFF)
			cpuRegs.CP0.n.Index = 0x80000000;
		LogTLBEvent("TLBP_RES", (cpuRegs.CP0.n.Index & 0x80000000) ? -1 : static_cast<s32>(cpuRegs.CP0.n.Index & 0x3f));
	}

	void MFC0()
	{
		// Note on _Rd_ Condition 9: CP0.Count should be updated even if _Rt_ is 0.
		if ((_Rd_ != 9) && !_Rt_)
			return;

		//if(bExecBIOS == FALSE && _Rd_ == 25) Console.WriteLn("MFC0 _Rd_ %x = %x", _Rd_, cpuRegs.CP0.r[_Rd_]);
		switch (_Rd_)
		{
			case 12:
				cpuRegs.GPR.r[_Rt_].SD[0] = (s32)(cpuRegs.CP0.r[_Rd_] & 0xf0c79c1f);
				break;

			case 25:
				if (0 == (_Imm_ & 1)) // MFPS, register value ignored
				{
					cpuRegs.GPR.r[_Rt_].SD[0] = (s32)cpuRegs.PERF.n.pccr.val;
				}
				else if (0 == (_Imm_ & 2)) // MFPC 0, only LSB of register matters
				{
					COP0_UpdatePCCR();
					cpuRegs.GPR.r[_Rt_].SD[0] = (s32)cpuRegs.PERF.n.pcr0;
				}
				else // MFPC 1
				{
					COP0_UpdatePCCR();
					cpuRegs.GPR.r[_Rt_].SD[0] = (s32)cpuRegs.PERF.n.pcr1;
				}
				/*Console.WriteLn("MFC0 PCCR = %x PCR0 = %x PCR1 = %x IMM= %x",  params
cpuRegs.PERF.n.pccr, cpuRegs.PERF.n.pcr0, cpuRegs.PERF.n.pcr1, _Imm_ & 0x3F);*/
				break;

			case 24:
				COP0_LOG("MFC0 Breakpoint debug Registers code = %x", cpuRegs.code & 0x3FF);
				break;

			case 9:
			{
				u32 incr = cpuRegs.cycle - cpuRegs.lastCOP0Cycle;
				if (incr == 0)
					incr++;
				cpuRegs.CP0.n.Count += incr;
				cpuRegs.lastCOP0Cycle = cpuRegs.cycle;
				if (!_Rt_)
					break;
			}
				[[fallthrough]];

			default:
				cpuRegs.GPR.r[_Rt_].SD[0] = (s32)cpuRegs.CP0.r[_Rd_];
		}
	}

	void MTC0()
	{
		//if(bExecBIOS == FALSE && _Rd_ == 25) Console.WriteLn("MTC0 _Rd_ %x = %x", _Rd_, cpuRegs.CP0.r[_Rd_]);
		switch (_Rd_)
		{
			case 0:
				WriteCP0Index(cpuRegs.GPR.r[_Rt_].UL[0]);
				break;

			case 1:
				WriteCP0Random(cpuRegs.GPR.r[_Rt_].UL[0]);
				break;

			case 2:
				WriteCP0EntryLo0(cpuRegs.GPR.r[_Rt_].UL[0]);
				break;

			case 3:
				WriteCP0EntryLo1(cpuRegs.GPR.r[_Rt_].UL[0]);
				break;

			case 4:
				WriteCP0Context(cpuRegs.GPR.r[_Rt_].UL[0]);
				break;

			case 5:
				WriteCP0PageMask(cpuRegs.GPR.r[_Rt_].UL[0]);
				break;

			case 6:
				WriteCP0Wired(cpuRegs.GPR.r[_Rt_].UL[0]);
				break;

			case 9:
				cpuRegs.lastCOP0Cycle = cpuRegs.cycle;
				cpuRegs.CP0.r[9] = cpuRegs.GPR.r[_Rt_].UL[0];
				break;

			case 10:
				WriteCP0EntryHi(cpuRegs.GPR.r[_Rt_].UL[0]);
				break;

			case 12:
				WriteCP0Status(cpuRegs.GPR.r[_Rt_].UL[0]);
				break;

			case 16:
				WriteCP0Config(cpuRegs.GPR.r[_Rt_].UL[0]);
				break;

			case 24:
				COP0_LOG("MTC0 Breakpoint debug Registers code = %x", cpuRegs.code & 0x3FF);
				break;

			case 25:
				/*if(bExecBIOS == FALSE && _Rd_ == 25) Console.WriteLn("MTC0 PCCR = %x PCR0 = %x PCR1 = %x IMM= %x", params
	cpuRegs.PERF.n.pccr, cpuRegs.PERF.n.pcr0, cpuRegs.PERF.n.pcr1, _Imm_ & 0x3F);*/
				if (0 == (_Imm_ & 1)) // MTPS
				{
					if (0 != (_Imm_ & 0x3E)) // only effective when the register is 0
						break;
					// Updates PCRs and sets the PCCR.
					COP0_UpdatePCCR();
					cpuRegs.PERF.n.pccr.val = cpuRegs.GPR.r[_Rt_].UL[0];
					COP0_DiagnosticPCCR();
				}
				else if (0 == (_Imm_ & 2)) // MTPC 0, only LSB of register matters
				{
					cpuRegs.PERF.n.pcr0 = cpuRegs.GPR.r[_Rt_].UL[0];
					cpuRegs.lastPERFCycle[0] = cpuRegs.cycle;
				}
				else // MTPC 1
				{
					cpuRegs.PERF.n.pcr1 = cpuRegs.GPR.r[_Rt_].UL[0];
					cpuRegs.lastPERFCycle[1] = cpuRegs.cycle;
				}
				break;

			default:
				cpuRegs.CP0.r[_Rd_] = cpuRegs.GPR.r[_Rt_].UL[0];
				break;
		}
	}

	int CPCOND0()
	{
		return (((dmacRegs.stat.CIS | ~dmacRegs.pcr.CPC) & 0x3FF) == 0x3ff);
	}

	//#define CPCOND0	1

	void BC0F()
	{
		if (CPCOND0() == 0)
			intDoBranch(_BranchTarget_);
	}

	void BC0T()
	{
		if (CPCOND0() == 1)
			intDoBranch(_BranchTarget_);
	}

	void BC0FL()
	{
		if (CPCOND0() == 0)
			intDoBranch(_BranchTarget_);
		else
			cpuRegs.pc += 4;
	}

	void BC0TL()
	{
		if (CPCOND0() == 1)
			intDoBranch(_BranchTarget_);
		else
			cpuRegs.pc += 4;
	}

	void ERET()
	{
#ifdef ENABLE_VTUNE
		// Allow to stop vtune in a predictable way to compare runs
		// Of course, the limit will depend on the game.
		const u32 million = 1000 * 1000;
		static u32 vtune = 0;
		vtune++;

		// quick_exit vs exit: quick_exit won't call static storage destructor (OS will manage). It helps
		// avoiding the race condition between threads destruction.
		if (vtune > 30 * million)
		{
			Console.WriteLn("VTUNE: quick_exit");
			std::quick_exit(EXIT_SUCCESS);
		}
		else if (!(vtune % million))
		{
			Console.WriteLn("VTUNE: ERET was called %uM times", vtune / million);
		}

#endif

		if (cpuRegs.CP0.n.Status.b.ERL)
		{
			cpuRegs.pc = cpuRegs.CP0.n.ErrorEPC;
			cpuRegs.CP0.n.Status.b.ERL = 0;
		}
		else
		{
			cpuRegs.pc = cpuRegs.CP0.n.EPC;
			cpuRegs.CP0.n.Status.b.EXL = 0;
		}
		cpuUpdateOperationMode();
		cpuSetNextEventDelta(4);
		intSetBranch();
	}

	void DI()
	{
		if (cpuRegs.CP0.n.Status.b._EDI || cpuRegs.CP0.n.Status.b.EXL ||
			cpuRegs.CP0.n.Status.b.ERL || (cpuRegs.CP0.n.Status.b.KSU == 0))
		{
			cpuRegs.CP0.n.Status.b.EIE = 0;
			// IRQs are disabled so no need to do a cpu exception/event test...
			//cpuSetNextEventDelta();
		}
	}

	void EI()
	{
		if (cpuRegs.CP0.n.Status.b._EDI || cpuRegs.CP0.n.Status.b.EXL ||
			cpuRegs.CP0.n.Status.b.ERL || (cpuRegs.CP0.n.Status.b.KSU == 0))
		{
			cpuRegs.CP0.n.Status.b.EIE = 1;
			// schedule an event test, which will check for and raise pending IRQs.
			cpuSetNextEventDelta(4);
		}
	}

} // namespace COP0
} // namespace OpcodeImpl
} // namespace Interpreter
} // namespace R5900
