#!/usr/bin/env python3
from pathlib import Path
import argparse
import shutil
import sys

ROOT = Path(__file__).resolve().parent
TARGET = ROOT / "pcsx2" / "vtlb.cpp"
BACKUP = TARGET.with_name("vtlb.cpp.pre-tlbmod-opcode-probe")

BASE = r'''static __ri void vtlb_Modified(u32 addr)
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

EXACT = r'''static __ri void vtlb_Modified(u32 addr)
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

BROAD = r'''static __ri void vtlb_Modified(u32 addr)
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


def die(msg: str) -> None:
    print(f"ERROR: {msg}", file=sys.stderr)
    raise SystemExit(1)


def apply() -> None:
    if not TARGET.exists():
        die(f"Could not find {TARGET}")

    text = TARGET.read_text(encoding="utf-8")

    if BROAD in text:
        print("Broad TLBMOD probe is already applied.")
        return

    if EXACT in text:
        # The first probe already made the backup. Preserve it so --restore
        # still returns to the exact pre-probe working tree.
        TARGET.write_text(text.replace(EXACT, BROAD, 1), encoding="utf-8")
        print(f"Upgraded exact-match probe to broad TLBMOD trace in:\n  {TARGET}")
        print("Existing pre-probe backup was preserved.")
        return

    if BASE in text:
        if BACKUP.exists():
            die(f"Backup already exists: {BACKUP}. Refusing to overwrite it.")
        shutil.copy2(TARGET, BACKUP)
        TARGET.write_text(text.replace(BASE, BROAD, 1), encoding="utf-8")
        print(f"Applied broad PSBBN TLBMOD trace to:\n  {TARGET}")
        print(f"Backup saved as:\n  {BACKUP}")
        return

    die("Expected vtlb_Modified() block was not found. Refusing to modify the file.")


def restore() -> None:
    if not BACKUP.exists():
        die(f"Backup not found: {BACKUP}")

    shutil.copy2(BACKUP, TARGET)
    BACKUP.unlink()
    print(f"Restored exact pre-probe source:\n  {TARGET}")


def main() -> None:
    parser = argparse.ArgumentParser(description="Apply or restore the PSBBN broad TLB Modified probe.")
    parser.add_argument("--restore", action="store_true", help="restore vtlb.cpp from the original pre-probe backup")
    args = parser.parse_args()

    if args.restore:
        restore()
    else:
        apply()


if __name__ == "__main__":
    main()
