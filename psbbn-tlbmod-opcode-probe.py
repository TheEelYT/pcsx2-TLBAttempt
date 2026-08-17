#!/usr/bin/env python3
from pathlib import Path
import argparse
import shutil
import sys

ROOT = Path(__file__).resolve().parent
VTLB = ROOT / "pcsx2" / "vtlb.cpp"
R5900 = ROOT / "pcsx2" / "R5900.cpp"
VTLB_BACKUP = VTLB.with_name("vtlb.cpp.pre-tlbmod-opcode-probe")
R5900_BACKUP = R5900.with_name("R5900.cpp.pre-tlb-exception-probe")

VTLB_BASE = r'''static __ri void vtlb_Modified(u32 addr)
{
	if (Cpu == &intCpu)
	{
		cpuTlbModified(addr, cpuRegs.branch);
		Cpu->CancelInstruction();
		return;
	}

	static int spamStop = 0;
	if (spamStop++ < 50 || IsDevBuild)
		Console.Error("TLB Modified, pc=0x%x addr=0x%x", cpuRegs.pc, addr);
}
'''

VTLB_EXACT = r'''static __ri void vtlb_Modified(u32 addr)
{
	// Temporary PSBBN probe: capture the raw EE opcode for the repeating
	// TLB Modified fault we've isolated in the PSBBN userspace process.
	if (cpuRegs.pc == 0x0FB64BF8 && addr == 0x0FB60364)
	{
		Console.Error(
			"PSBBN TLBMOD: pc=%08X code=%08X addr=%08X ASID=%02X EntryHi=%08X EntryLo0=%08X EntryLo1=%08X",
			cpuRegs.pc,
			cpuRegs.code,
			addr,
			cpuRegs.CP0.n.EntryHi & 0xff,
			cpuRegs.CP0.n.EntryHi,
			cpuRegs.CP0.n.EntryLo0,
			cpuRegs.CP0.n.EntryLo1);
	}

	if (Cpu == &intCpu)
	{
		cpuTlbModified(addr, cpuRegs.branch);
		Cpu->CancelInstruction();
		return;
	}

	static int spamStop = 0;
	if (spamStop++ < 50 || IsDevBuild)
		Console.Error("TLB Modified, pc=0x%x addr=0x%x", cpuRegs.pc, addr);
}
'''

VTLB_BROAD = r'''static __ri void vtlb_Modified(u32 addr)
{
	// Temporary PSBBN probe. Do not assume the saved fault pair repeats:
	// log the first TLB Modified events so we can identify the actual hot fault.
	static int psbbnTlbmodTraceCount = 0;
	if (psbbnTlbmodTraceCount++ < 256)
	{
		Console.Error(
			"PSBBN TLBMOD[%03d]: core=%s pc=%08X code=%08X addr=%08X EPC=%08X Cause=%08X ASID=%02X EntryHi=%08X EntryLo0=%08X EntryLo1=%08X",
			psbbnTlbmodTraceCount,
			(Cpu == &intCpu) ? "INT" : "REC",
			cpuRegs.pc,
			cpuRegs.code,
			addr,
			cpuRegs.CP0.n.EPC,
			cpuRegs.CP0.n.Cause,
			cpuRegs.CP0.n.EntryHi & 0xff,
			cpuRegs.CP0.n.EntryHi,
			cpuRegs.CP0.n.EntryLo0,
			cpuRegs.CP0.n.EntryLo1);
	}

	if (Cpu == &intCpu)
	{
		cpuTlbModified(addr, cpuRegs.branch);
		Cpu->CancelInstruction();
		return;
	}

	static int spamStop = 0;
	if (spamStop++ < 50 || IsDevBuild)
		Console.Error("TLB Modified, pc=0x%x addr=0x%x", cpuRegs.pc, addr);
}
'''

R5900_BASE = r'''void cpuTlbMiss(u32 addr, u32 bd, u32 excode)
{
	// Must be decided before EntryHi is rewritten below, since the ASID in it
	// is what the match is against.
	eeTlbInvalidMatch = eeTlbMatches(addr);

	// Avoid too much spamming on the interpreter
	if (Cpu != &intCpu || IsDebugBuild) {
		Console.Error("cpuTlbMiss pc:%x, cycl:%llx, addr: %x, status=%x, code=%x",
				cpuRegs.pc, cpuRegs.cycle, addr, cpuRegs.CP0.n.Status.val, excode);
	}

	cpuRegs.CP0.n.BadVAddr = addr;
	cpuRegs.CP0.n.Context &= 0xFF80000F;
	cpuRegs.CP0.n.Context |= (addr >> 9) & 0x007FFFF0;
	cpuRegs.CP0.n.EntryHi = (addr & 0xFFFFE000) | (cpuRegs.CP0.n.EntryHi & 0x1FFF);

	cpuRegs.pc -= 4;
	cpuException(excode, bd);
}
'''

R5900_PROBE = r'''void cpuTlbMiss(u32 addr, u32 bd, u32 excode)
{
	// Must be decided before EntryHi is rewritten below, since the ASID in it
	// is what the match is against.
	eeTlbInvalidMatch = eeTlbMatches(addr);

	// Temporary PSBBN probe: capture the actual TLB exception being raised,
	// rather than stale CP0 state left by the previous exception. Restrict it
	// to the 0x0FBxxxxx userspace region where the PSBBN fault occurs.
	static int psbbnTlbExTraceCount = 0;
	const u32 psbbnExc = excode & 0x7C;
	const char* psbbnKind = (psbbnExc == EXC_CODE(1)) ? "MOD" :
		(psbbnExc == EXC_CODE(2)) ? "TLBL" :
		(psbbnExc == EXC_CODE(3)) ? "TLBS" : "OTHER";
	const u32 psbbnIncomingPc = cpuRegs.pc;
	const u32 psbbnIncomingCode = cpuRegs.code;
	const u32 psbbnIncomingEntryHi = cpuRegs.CP0.n.EntryHi;
	const u32 psbbnIncomingAsid = psbbnIncomingEntryHi & 0xff;
	const bool psbbnInRegion = ((psbbnIncomingPc & 0xFFF00000u) == 0x0FB00000u) ||
		((addr & 0xFFF00000u) == 0x0FB00000u);
	const bool psbbnTrace = psbbnInRegion && psbbnTlbExTraceCount < 512;
	const int psbbnTraceId = psbbnTrace ? ++psbbnTlbExTraceCount : 0;

	if (psbbnTrace)
	{
		Console.Error(
			"PSBBN TLBEX[%03d] PRE kind=%s core=%s pc=%08X code=%08X addr=%08X bd=%u ASID=%02X invalid=%u EntryHi=%08X",
			psbbnTraceId,
			psbbnKind,
			(Cpu == &intCpu) ? "INT" : "REC",
			psbbnIncomingPc,
			psbbnIncomingCode,
			addr,
			bd,
			psbbnIncomingAsid,
			eeTlbInvalidMatch ? 1u : 0u,
			psbbnIncomingEntryHi);
	}

	// Avoid too much spamming on the interpreter
	if (Cpu != &intCpu || IsDebugBuild) {
		Console.Error("cpuTlbMiss pc:%x, cycl:%llx, addr: %x, status=%x, code=%x",
				cpuRegs.pc, cpuRegs.cycle, addr, cpuRegs.CP0.n.Status.val, excode);
	}

	cpuRegs.CP0.n.BadVAddr = addr;
	cpuRegs.CP0.n.Context &= 0xFF80000F;
	cpuRegs.CP0.n.Context |= (addr >> 9) & 0x007FFFF0;
	cpuRegs.CP0.n.EntryHi = (addr & 0xFFFFE000) | (cpuRegs.CP0.n.EntryHi & 0x1FFF);

	cpuRegs.pc -= 4;
	cpuException(excode, bd);

	if (psbbnTrace)
	{
		Console.Error(
			"PSBBN TLBEX[%03d] POST kind=%s EPC=%08X Cause=%08X BadVAddr=%08X EntryHi=%08X vectorPC=%08X",
			psbbnTraceId,
			psbbnKind,
			cpuRegs.CP0.n.EPC,
			cpuRegs.CP0.n.Cause,
			cpuRegs.CP0.n.BadVAddr,
			cpuRegs.CP0.n.EntryHi,
			cpuRegs.pc);
	}
}
'''


def die(msg: str) -> None:
    print(f"ERROR: {msg}", file=sys.stderr)
    raise SystemExit(1)


def apply() -> None:
    if not VTLB.exists():
        die(f"Could not find {VTLB}")
    if not R5900.exists():
        die(f"Could not find {R5900}")

    vtlb_text = VTLB.read_text(encoding="utf-8")
    r5900_text = R5900.read_text(encoding="utf-8")

    # Validate everything before changing either file.
    if VTLB_BROAD in vtlb_text:
        new_vtlb_text = vtlb_text.replace(VTLB_BROAD, VTLB_BASE, 1)
        vtlb_action = "Removed broad vtlb_Modified() logger"
    elif VTLB_EXACT in vtlb_text:
        new_vtlb_text = vtlb_text.replace(VTLB_EXACT, VTLB_BASE, 1)
        vtlb_action = "Removed exact-match vtlb_Modified() logger"
    elif VTLB_BASE in vtlb_text:
        new_vtlb_text = vtlb_text
        vtlb_action = "vtlb_Modified() already clean"
    else:
        die("Expected vtlb_Modified() block was not found. Refusing to modify files.")

    if R5900_PROBE in r5900_text:
        new_r5900_text = r5900_text
        r5900_action = "TLB exception probe is already applied"
    elif R5900_BASE in r5900_text:
        if R5900_BACKUP.exists():
            die(f"Backup already exists: {R5900_BACKUP}. Refusing to overwrite it.")
        new_r5900_text = r5900_text.replace(R5900_BASE, R5900_PROBE, 1)
        r5900_action = "Applied central TLBL/TLBS/MOD probe"
    else:
        die("Expected cpuTlbMiss() block was not found. Refusing to modify files.")

    if R5900_PROBE not in r5900_text:
        shutil.copy2(R5900, R5900_BACKUP)

    if new_vtlb_text != vtlb_text:
        VTLB.write_text(new_vtlb_text, encoding="utf-8")
    if new_r5900_text != r5900_text:
        R5900.write_text(new_r5900_text, encoding="utf-8")

    print(vtlb_action + ":")
    print(f"  {VTLB}")
    print(r5900_action + ":")
    print(f"  {R5900}")
    if R5900_BACKUP.exists():
        print("R5900 pre-probe backup:")
        print(f"  {R5900_BACKUP}")
    if VTLB_BACKUP.exists():
        print("Original pre-TLBMOD vtlb backup preserved:")
        print(f"  {VTLB_BACKUP}")
    print("Rebuild PCSX2 and look for 'PSBBN TLBEX[' lines.")


def restore() -> None:
    restored = False

    if R5900_BACKUP.exists():
        shutil.copy2(R5900_BACKUP, R5900)
        R5900_BACKUP.unlink()
        print(f"Restored exact pre-probe R5900.cpp:\n  {R5900}")
        restored = True

    if VTLB_BACKUP.exists():
        shutil.copy2(VTLB_BACKUP, VTLB)
        VTLB_BACKUP.unlink()
        print(f"Restored exact pre-TLBMOD vtlb.cpp:\n  {VTLB}")
        restored = True

    if not restored:
        die("No probe backups were found; nothing to restore.")


def main() -> None:
    parser = argparse.ArgumentParser(description="Apply or restore the PSBBN EE TLB exception probe.")
    parser.add_argument("--restore", action="store_true", help="restore R5900.cpp and vtlb.cpp from their original pre-probe backups")
    args = parser.parse_args()

    if args.restore:
        restore()
    else:
        apply()


if __name__ == "__main__":
    main()
