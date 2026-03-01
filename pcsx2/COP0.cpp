// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Common.h"
#include "COP0.h"

#ifdef PCSX2_DEVBUILD
#include <unordered_set>
#endif

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


static __fi u32 COP0_GetSanitizedWired()
{
	return std::min(cpuRegs.CP0.n.Wired & 0x3f, 47u);
}

static __fi u32 COP0_SanitizeRandomForWired(u32 random, u32 wired)
{
	const u32 rand_index = random & 0x3f;
	if (rand_index < wired || rand_index > 47)
		return 47;

	return rand_index;
}

static __fi u32 COP0_GetEntryHiVPN2(u32 entry_hi)
{
	// EE EntryHi uses VPN2[31:13], ASID[7:0], with bits [12:8] treated as region/ignored in TLB matching.
	return (entry_hi >> 13) & 0x7ffff;
}

static __fi u32 COP0_GetTLBCompareMask(const tlbs& entry)
{
	// PageMask bits select VPN2 bits to ignore during probe/lookup.
	return (~entry.Mask()) & 0x7ffff;
}

struct COP0TLBMatchResult
{
	bool matched;
	bool odd_page;
};

static __fi COP0TLBMatchResult COP0_TLBMatchTuple(const tlbs& entry, u32 entry_hi, u32 vaddr, bool check_asid)
{
	const u32 compare_mask = COP0_GetTLBCompareMask(entry);
	const bool vpn2_match = (entry.EntryHi.VPN2 & compare_mask) == (COP0_GetEntryHiVPN2(entry_hi) & compare_mask);
	const bool asid_match = !check_asid || entry.isGlobal() || (entry.EntryHi.ASID == static_cast<u8>(entry_hi));

	const u32 page_span = entry.Mask() + 1;
	const bool odd_page = (((vaddr >> 12) & page_span) != 0);
	return {vpn2_match && asid_match, odd_page};
}

static __fi COP0TLBMatchResult COP0_TLBEntryMatchesVaddr(const tlbs& entry, u32 vaddr)
{
	const u32 entry_hi = (vaddr & 0xffffe000) | entry.EntryHi.ASID;
	return COP0_TLBMatchTuple(entry, entry_hi, vaddr, false);
}

static __fi bool COP0_IsTLBManagedVaddr(u32 vaddr)
{
	const u32 segment_class = vaddr >> 29;
	return (segment_class != 4) && (segment_class != 5);
}

#ifdef PCSX2_DEVBUILD
static void COP0_LogTLBProtectedSegmentWarning(const tlbs& t, int index, u32 vaddr, bool map)
{
	static bool s_logged = false;
	if (s_logged)
		return;
	s_logged = true;

	DevCon.Warning("COP0: Skipping TLB %s targeting protected direct-mapped segment (vaddr=0x%08X, index=%d, EntryHi=0x%08X, PageMask=0x%08X, EntryLo0=0x%08X, EntryLo1=0x%08X)",
		map ? "map" : "clear", vaddr, index, t.EntryHi.UL, t.PageMask.UL, t.EntryLo0.UL, t.EntryLo1.UL);
}
#endif

#ifdef PCSX2_DEVBUILD
static void COP0_LogSkippedTLBPageOpOnce(int index, u32 vaddr, bool map, bool odd_page)
{
	static std::unordered_set<u64> s_logged_signatures;
	const u64 signature = (static_cast<u64>(map) << 63) |
		(static_cast<u64>(odd_page) << 62) |
		(static_cast<u64>(index & 0xff) << 32) |
		static_cast<u64>(vaddr);

	if (!s_logged_signatures.insert(signature).second)
		return;

	DevCon.Warning("COP0: Skipping %s TLB page op for non-TLB-managed vaddr 0x%08X (index=%d, page=%u)",
		map ? "map" : "unmap", vaddr, index, odd_page ? 1 : 0);
}
#endif

void COP0_SetWired(u32 value)
{
	cpuRegs.CP0.n.Wired = value & 0x3f;
	const u32 wired = COP0_GetSanitizedWired();
	cpuRegs.CP0.n.Random = COP0_SanitizeRandomForWired(cpuRegs.CP0.n.Random, wired);
}

void COP0_UpdateRandom(u32 cycle_delta)
{
	if (cycle_delta == 0)
		return;

	const u32 wired = COP0_GetSanitizedWired();
	if (wired >= 47)
	{
		cpuRegs.CP0.n.Random = 47;
		return;
	}

	const u32 span = 48 - wired;
	const u32 random = COP0_SanitizeRandomForWired(cpuRegs.CP0.n.Random, wired);
	const u32 position = random - wired;
	const u32 decremented = (position + span - (cycle_delta % span)) % span;
	cpuRegs.CP0.n.Random = wired + decremented;
}

void COP0_UpdateRandom()
{
	COP0_UpdateRandom(1);
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


void MapTLB(const tlbs& t, int i)
{
	u32 addr;
	u32 saddr, eaddr;

	COP0_LOG("MAP TLB %d: 0x%08X-> [0x%08X 0x%08X] S=%d G=%d ASID=%d Mask=0x%03X EntryLo0 PFN=%x EntryLo0 Cache=%x EntryLo1 PFN=%x EntryLo1 Cache=%x VPN2=%x",
		i, t.VPN2(), t.PFN0(), t.PFN1(), t.isSPR() >> 31, t.isGlobal(), t.EntryHi.ASID,
		t.Mask(), t.EntryLo0.PFN, t.EntryLo0.C, t.EntryLo1.PFN, t.EntryLo1.C, t.VPN2());

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
		saddr = t.VPN2() >> 12;
		eaddr = saddr + ((t.Mask() + 1) * 2);

		for (addr = saddr; addr < eaddr; addr++)
		{
			const u32 vaddr = addr << 12;
			if (!COP0_IsTLBManagedVaddr(vaddr))
			{
				const u32 vaddr = addr << 12;
				if (!COP0_IsTLBManagedVaddr(vaddr))
				{
#ifdef PCSX2_DEVBUILD
					COP0_LogSkippedTLBPageOpOnce(i, vaddr, true, false);
#endif
					continue;
				}

				if (COP0_TLBEntryMatchesVaddr(t, vaddr).matched)
				{ //match
					memSetPageAddr(vaddr, t.PFN0() + ((addr - saddr) << 12));
					Cpu->Clear(vaddr, 0x400);
				}
			}

			const COP0TLBMatchResult lookup = COP0_TLBEntryMatchesVaddr(t, vaddr);
#ifdef PCSX2_DEVBUILD
			const COP0TLBMatchResult probe = COP0_TLBMatchTuple(t, (vaddr & 0xffffe000) | t.EntryHi.ASID, vaddr, true);
			if (probe.matched != lookup.matched)
			{
				const u32 vaddr = addr << 12;
				if (!COP0_IsTLBManagedVaddr(vaddr))
				{
#ifdef PCSX2_DEVBUILD
					COP0_LogSkippedTLBPageOpOnce(i, vaddr, true, true);
#endif
					continue;
				}

				if (COP0_TLBEntryMatchesVaddr(t, vaddr).matched)
				{ //match
					memSetPageAddr(vaddr, t.PFN1() + ((addr - saddr) << 12));
					Cpu->Clear(vaddr, 0x400);
				}
			}
#endif
			const EntryLo_t& lo = use_odd_page ? t.EntryLo1 : t.EntryLo0;
			if (!lo.V)
				continue;

			const u32 range_base = saddr + (use_odd_page ? page_span : 0);
			const u32 paddr_base = use_odd_page ? t.PFN1() : t.PFN0();
			memSetPageAddr(vaddr, paddr_base + ((addr - range_base) << 12));
			Cpu->Clear(vaddr, 0x400);
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
	u32 addr;
	u32 saddr, eaddr;

	if (t.isSPR())
	{
		vtlb_VMapUnmap(t.VPN2(), 0x4000);
		return;
	}

	saddr = t.VPN2() >> 12;
	eaddr = saddr + ((t.Mask() + 1) * 2);
	for (addr = saddr; addr < eaddr; addr++)
		{
			const u32 vaddr = addr << 12;
			if (!COP0_IsTLBManagedVaddr(vaddr))
			{
#ifdef PCSX2_DEVBUILD
				COP0_LogSkippedTLBPageOpOnce(i, vaddr, false, false);
#endif
				continue;
			}

			if (COP0_TLBEntryMatchesVaddr(t, vaddr).matched)
			{ //match
				memClearPageAddr(vaddr);
				Cpu->Clear(vaddr, 0x400);
			}

	if (t.EntryLo1.V)
	{
		saddr = (t.VPN2() >> 12) + t.Mask() + 1;
		eaddr = saddr + t.Mask() + 1;
		//	Console.WriteLn("Clear TLB: %08x ~ %08x",saddr,eaddr-1);
		for (addr = saddr; addr < eaddr; addr++)
		{
			const u32 vaddr = addr << 12;
			if (!COP0_IsTLBManagedVaddr(vaddr))
			{
#ifdef PCSX2_DEVBUILD
				COP0_LogSkippedTLBPageOpOnce(i, vaddr, false, true);
#endif
				continue;
			}

			if (COP0_TLBEntryMatchesVaddr(t, vaddr).matched)
			{ //match
				memClearPageAddr(vaddr);
				Cpu->Clear(vaddr, 0x400);
			}
#endif
			if (!lookup.matched)
				continue;

#ifdef PCSX2_DEVBUILD
			const bool expected_odd_page = (addr - saddr) >= (t.Mask() + 1);
			if (expected_odd_page != lookup.odd_page)
			{
				DevCon.Warning(
					"COP0: TLB odd/even mismatch during unmap (vaddr=0x%08X, index=%d, EntryHi=0x%08X, PageMask=0x%08X, expected_odd=%d, helper_odd=%d)",
					vaddr, i, t.EntryHi.UL, t.PageMask.UL, expected_odd_page, lookup.odd_page);
				pxAssertRel(expected_odd_page == lookup.odd_page);
			}
#endif
			const EntryLo_t& lo = lookup.odd_page ? t.EntryLo1 : t.EntryLo0;
			if (!lo.V)
				continue;

			memClearPageAddr(vaddr);
			Cpu->Clear(vaddr, 0x400);
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
		COP0_LOG("COP0_TLBR %d:%x,%x,%x,%x",
			cpuRegs.CP0.n.Index, cpuRegs.CP0.n.PageMask, cpuRegs.CP0.n.EntryHi,
			cpuRegs.CP0.n.EntryLo0, cpuRegs.CP0.n.EntryLo1);

		const u8 i = cpuRegs.CP0.n.Index & 0x3f;

		if (i > 47)
		{
			Console.Warning("TLBR with index > 47! (%d)", i);
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
	}

	void TLBWI()
	{
		const u8 j = cpuRegs.CP0.n.Index & 0x3f;

		if (j > 47)
		{
			Console.Warning("TLBWI with index > 47! (%d)", j);
			return;
		}

		COP0_LOG("COP0_TLBWI %d:%x,%x,%x,%x",
			cpuRegs.CP0.n.Index, cpuRegs.CP0.n.PageMask, cpuRegs.CP0.n.EntryHi,
			cpuRegs.CP0.n.EntryLo0, cpuRegs.CP0.n.EntryLo1);
		if (TraceLogging.EE.Bios.IsActive())
		{
			DevCon.WriteLn(
				"TLBWI write index=%u BadVAddr=0x%08x Context=0x%08x EntryHi=0x%08x EPC=0x%08x EntryLo0=0x%08x EntryLo1=0x%08x",
				j, cpuRegs.CP0.n.BadVAddr, cpuRegs.CP0.n.Context, cpuRegs.CP0.n.EntryHi, cpuRegs.CP0.n.EPC,
				cpuRegs.CP0.n.EntryLo0, cpuRegs.CP0.n.EntryLo1);
		}

		UnmapTLB(tlb[j], j);
		WriteTLB(j);
	}

	void TLBWR()
	{
		const u32 wired = COP0_GetSanitizedWired();
		const u8 j = static_cast<u8>(COP0_SanitizeRandomForWired(cpuRegs.CP0.n.Random, wired));

		if (j < wired || j > 47)
		{
			Console.Warning("TLBWR selected invalid random index (%d), Wired=%d", j, wired);
			return;
		}

		DevCon.Warning("COP0_TLBWR %d:%x,%x,%x,%x\n",
			cpuRegs.CP0.n.Random, cpuRegs.CP0.n.PageMask, cpuRegs.CP0.n.EntryHi,
			cpuRegs.CP0.n.EntryLo0, cpuRegs.CP0.n.EntryLo1);
		if (TraceLogging.EE.Bios.IsActive())
		{
			DevCon.WriteLn(
				"TLBWR write random=%u index=%u BadVAddr=0x%08x Context=0x%08x EntryHi=0x%08x EPC=0x%08x EntryLo0=0x%08x EntryLo1=0x%08x",
				cpuRegs.CP0.n.Random, j, cpuRegs.CP0.n.BadVAddr, cpuRegs.CP0.n.Context, cpuRegs.CP0.n.EntryHi,
				cpuRegs.CP0.n.EPC, cpuRegs.CP0.n.EntryLo0, cpuRegs.CP0.n.EntryLo1);
		}

		UnmapTLB(tlb[j], j);
		WriteTLB(j);
	}

	void TLBP()
	{
		cpuRegs.CP0.n.Index = 0x80000000;
		for (u32 i = 0; i < 48; i++)
		{
			if (COP0_TLBMatchTuple(tlb[i], cpuRegs.CP0.n.EntryHi, cpuRegs.CP0.n.EntryHi & 0xffffe000, true).matched)
			{
				cpuRegs.CP0.n.Index = i;
				break;
			}
		}
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
				COP0_UpdateRandom(incr);
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
		const u32 value = cpuRegs.GPR.r[_Rt_].UL[0];

		switch (_Rd_)
		{
			case 0:
				// EE CP0.Index: only Index[5:0] is writable via MTC0.
				// The probe/fail state bit (P, bit 31) is produced by TLBP and remains read-only here.
				cpuRegs.CP0.n.Index = value & 0x3f;
				break;

			case 1:
				// EE CP0.Random: Random[5:0] is the only writable field. Keep it in-range for TLBWR.
				cpuRegs.CP0.n.Random = COP0_SanitizeRandomForWired(value & 0x3f, COP0_GetSanitizedWired());
				break;

			case 2:
				// EE CP0.EntryLo0: writable fields are G/V/D/C/PFN and S (scratchpad select, bit 31).
				// Reserved bits [30:26] are forced low.
				cpuRegs.CP0.n.EntryLo0 = value & 0x83ffffff;
				break;

			case 3:
				// EE CP0.EntryLo1: writable fields are G/V/D/C/PFN; upper reserved bits are fixed low.
				cpuRegs.CP0.n.EntryLo1 = value & 0x03ffffff;
				break;

			case 4:
				// EE CP0.Context: PTEBase[31:23] and low software bits [3:0] are writable.
				// BadVPN2[22:4] is maintained by fault/refill address generation paths.
				cpuRegs.CP0.n.Context = (cpuRegs.CP0.n.Context & 0x007ffff0) | (value & 0xff80000f);
				break;

			case 5:
				// EE CP0.PageMask: only Mask[24:13] is writable.
				cpuRegs.CP0.n.PageMask = value & 0x01ffe000;
				break;

			case 6:
				// EE CP0.Wired: Wired[5:0] is writable and immediately constrains Random.
				COP0_SetWired(value);
				break;

			case 10:
				// EE CP0.EntryHi: writable fields are VPN2[31:13] and ASID[7:0].
				// Keeping the register canonical avoids reserved-bit pollution in TLBP/TLBWR paths.
				cpuRegs.CP0.n.EntryHi = value & 0xffffe0ff;
				break;

			case 9:
				COP0_UpdateRandom(cpuRegs.cycle - cpuRegs.lastCOP0Cycle);
				cpuRegs.lastCOP0Cycle = cpuRegs.cycle;
				cpuRegs.CP0.r[9] = value;
				break;

			case 12:
				WriteCP0Status(value);
				break;

			case 16:
				WriteCP0Config(value);
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
					cpuRegs.PERF.n.pccr.val = value;
					COP0_DiagnosticPCCR();
				}
				else if (0 == (_Imm_ & 2)) // MTPC 0, only LSB of register matters
				{
					cpuRegs.PERF.n.pcr0 = value;
					cpuRegs.lastPERFCycle[0] = cpuRegs.cycle;
				}
				else // MTPC 1
				{
					cpuRegs.PERF.n.pcr1 = value;
					cpuRegs.lastPERFCycle[1] = cpuRegs.cycle;
				}
				break;

			default:
				cpuRegs.CP0.r[_Rd_] = value;
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
