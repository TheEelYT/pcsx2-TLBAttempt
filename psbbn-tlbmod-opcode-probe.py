#!/usr/bin/env python3
from pathlib import Path
import argparse
import shutil
import sys

ROOT = Path(__file__).resolve().parent
TARGET = ROOT / "pcsx2" / "vtlb.cpp"
BACKUP = TARGET.with_name("vtlb.cpp.pre-tlbmod-opcode-probe")

OLD = r'''static __ri void vtlb_Modified(u32 addr)
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

NEW = r'''static __ri void vtlb_Modified(u32 addr)
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


def die(msg: str) -> None:
    print(f"ERROR: {msg}", file=sys.stderr)
    raise SystemExit(1)


def apply() -> None:
    if not TARGET.exists():
        die(f"Could not find {TARGET}")

    text = TARGET.read_text(encoding="utf-8")

    if NEW in text:
        print("TLBMOD opcode probe is already applied.")
        return

    if OLD not in text:
        die("Expected vtlb_Modified() block was not found. Refusing to modify the file.")

    if BACKUP.exists():
        die(f"Backup already exists: {BACKUP}. Restore or remove it before applying again.")

    shutil.copy2(TARGET, BACKUP)
    TARGET.write_text(text.replace(OLD, NEW, 1), encoding="utf-8")

    print(f"Applied targeted PSBBN TLBMOD opcode probe to:\n  {TARGET}")
    print(f"Backup saved as:\n  {BACKUP}")
    print("Rebuild PCSX2, reproduce the PSBBN hang, then look for:")
    print("  PSBBN TLBMOD: pc=0FB64BF8 code=........ addr=0FB60364 ...")


def restore() -> None:
    if not BACKUP.exists():
        die(f"Backup not found: {BACKUP}")

    shutil.copy2(BACKUP, TARGET)
    BACKUP.unlink()
    print(f"Restored original source:\n  {TARGET}")


def main() -> None:
    parser = argparse.ArgumentParser(description="Apply or restore the PSBBN TLB Modified opcode probe.")
    parser.add_argument("--restore", action="store_true", help="restore vtlb.cpp from the probe backup")
    args = parser.parse_args()

    if args.restore:
        restore()
    else:
        apply()


if __name__ == "__main__":
    main()
