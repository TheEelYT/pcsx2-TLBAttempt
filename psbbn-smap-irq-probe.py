#!/usr/bin/env python3
from pathlib import Path
import shutil
import sys

ROOT = Path(__file__).resolve().parent
DEV9 = ROOT / "pcsx2" / "DEV9" / "DEV9.cpp"
SMAP = ROOT / "pcsx2" / "DEV9" / "smap.cpp"
BACKUPS = {
    DEV9: DEV9.with_name("DEV9.cpp.pre-smap-irq-probe"),
    SMAP: SMAP.with_name("smap.cpp.pre-smap-irq-probe"),
}


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{label}: expected exactly one match, found {count}")
    return text.replace(old, new, 1)


def restore() -> None:
    restored = False
    for path, backup in BACKUPS.items():
        if backup.exists():
            shutil.copy2(backup, path)
            backup.unlink()
            print(f"Restored {path}")
            restored = True
    if not restored:
        print("No SMAP IRQ probe backups were found; nothing to restore.")


def apply() -> None:
    for path, backup in BACKUPS.items():
        if not path.exists():
            raise RuntimeError(f"Missing source file: {path}")
        if backup.exists():
            raise RuntimeError(
                f"Backup already exists: {backup}\n"
                "The probe may already be applied. Use --restore first if needed."
            )
        shutil.copy2(path, backup)

    try:
        dev9 = DEV9.read_text(encoding="utf-8")
        smap = SMAP.read_text(encoding="utf-8")

        dev9 = replace_once(
            dev9,
            """void _DEV9irq(int cause, int cycles)\n{\n\t//DevCon.WriteLn(\"DEV9: _DEV9irq %x, %x\", cause, dev9.irqmask);\n\n\tdev9.irqcause |= cause;\n\n\tif (cycles < 1)\n\t\tdev9Irq(1);\n\telse\n\t\tdev9Irq(cycles);\n}\n""",
            """void _DEV9irq(int cause, int cycles)\n{\n\tconst u16 old_cause = dev9.irqcause;\n\tdev9.irqcause |= cause;\n\n\tif (cause & SMAP_INTR_BITMSK)\n\t{\n\t\tDevCon.WriteLn(\"[SMAPIRQ] raise cause=%04x cycles=%d old=%04x new=%04x mask=%04x pending=%04x\",\n\t\t\tcause, cycles, old_cause, dev9.irqcause, dev9.irqmask, dev9.irqcause & dev9.irqmask);\n\t}\n\n\tif (cycles < 1)\n\t\tdev9Irq(1);\n\telse\n\t\tdev9Irq(cycles);\n}\n""",
            "_DEV9irq",
        )

        dev9 = replace_once(
            dev9,
            """\t\tcase SPD_R_INTR_STAT:\n\t\t\t//DevCon.WriteLn(\"DEV9: SPD_R_INTR_STAT %dbit read %x\", width, dev9.irqcause);\n\t\t\treturn dev9.irqcause;\n\n\t\tcase SPD_R_INTR_MASK:\n\t\t\t//DevCon.WriteLn(\"DEV9: SPD_R_INTR_MASK %dbit read %x\", width, dev9.irqmask);\n\t\t\treturn dev9.irqmask;\n""",
            """\t\tcase SPD_R_INTR_STAT:\n\t\t\tDevCon.WriteLn(\"[SMAPIRQ] SPD_STAT read width=%d cause=%04x mask=%04x pending=%04x\",\n\t\t\t\twidth, dev9.irqcause, dev9.irqmask, dev9.irqcause & dev9.irqmask);\n\t\t\treturn dev9.irqcause;\n\n\t\tcase SPD_R_INTR_MASK:\n\t\t\tDevCon.WriteLn(\"[SMAPIRQ] SPD_MASK read width=%d mask=%04x cause=%04x pending=%04x\",\n\t\t\t\twidth, dev9.irqmask, dev9.irqcause, dev9.irqcause & dev9.irqmask);\n\t\t\treturn dev9.irqmask;\n""",
            "SpeedRead interrupt registers",
        )

        dev9 = replace_once(
            dev9,
            """\t\tcase SPD_R_REV_3:\n\t\t\t// The Expansion bay always says HDD and Ethernet are supported, we need to keep HDD enabled and we handle it elsewhere.\n\t\t\t// Ethernet we will turn off as not sure on what that would do right now, but no known game cares if it's off.\n\t\t\tif (EmuConfig.DEV9.EthEnable)\n\t\t\t\thard |= SPD_CAPS_SMAP;\n\n\t\t\t// TODO: Do we need flash? my 50003 model doesn't report this, but it does report DVR capable aka (1<<4), was that intended?\n\t\t\thard |= SPD_CAPS_ATA | SPD_CAPS_FLASH;\n\t\t\t//DevCon.WriteLn(\"DEV9: SPD_R_REV_3 %dbit read %x\", width, hard);\n\t\t\treturn hard;\n""",
            """\t\tcase SPD_R_REV_3:\n\t\t\t// The Expansion bay always says HDD and Ethernet are supported, we need to keep HDD enabled and we handle it elsewhere.\n\t\t\t// Ethernet we will turn off as not sure on what that would do right now, but no known game cares if it's off.\n\t\t\tif (EmuConfig.DEV9.EthEnable)\n\t\t\t\thard |= SPD_CAPS_SMAP;\n\n\t\t\t// TODO: Do we need flash? my 50003 model doesn't report this, but it does report DVR capable aka (1<<4), was that intended?\n\t\t\thard |= SPD_CAPS_ATA | SPD_CAPS_FLASH;\n\t\t\tDevCon.WriteLn(\"[SMAPIRQ] SPD_REV3 read width=%d value=%04x eth=%d\",\n\t\t\t\twidth, hard, EmuConfig.DEV9.EthEnable ? 1 : 0);\n\t\t\treturn hard;\n""",
            "SPD_R_REV_3",
        )

        dev9 = replace_once(
            dev9,
            """\t\tcase SPD_R_INTR_STAT:\n\t\t\tConsole.Error(\"DEV9: SPD_R_INTR_STAT %dbit write, WTF? %x\", width, value);\n\t\t\tdev9.irqcause = value;\n\t\t\treturn;\n\t\tcase SPD_R_INTR_MASK: // 8bit writes affect whole reg\n\t\t\t//DevCon.WriteLn(\"DEV9: SPD_R_INTR_MASK %dbit write %x\", checking for masked/unmasked interrupts\", width, value);\n\t\t\tif ((dev9.irqmask != value) && ((dev9.irqmask | value) & dev9.irqcause))\n\t\t\t{\n\t\t\t\t//DevCon.WriteLn(\"DEV9: SPD_R_INTR_MASK firing unmasked interrupts\");\n\t\t\t\tdev9Irq(1);\n\t\t\t}\n\t\t\tdev9.irqmask = value;\n\t\t\treturn;\n""",
            """\t\tcase SPD_R_INTR_STAT:\n\t\t\tConsole.Error(\"DEV9: SPD_R_INTR_STAT %dbit write, WTF? %x\", width, value);\n\t\t\tDevCon.WriteLn(\"[SMAPIRQ] SPD_STAT write width=%d value=%04x oldcause=%04x mask=%04x\",\n\t\t\t\twidth, value, dev9.irqcause, dev9.irqmask);\n\t\t\tdev9.irqcause = value;\n\t\t\tDevCon.WriteLn(\"[SMAPIRQ] SPD_STAT after cause=%04x mask=%04x pending=%04x\",\n\t\t\t\tdev9.irqcause, dev9.irqmask, dev9.irqcause & dev9.irqmask);\n\t\t\treturn;\n\t\tcase SPD_R_INTR_MASK: // 8bit writes affect whole reg\n\t\t\tDevCon.WriteLn(\"[SMAPIRQ] SPD_MASK write width=%d value=%04x oldmask=%04x cause=%04x pending_before=%04x\",\n\t\t\t\twidth, value, dev9.irqmask, dev9.irqcause, dev9.irqcause & dev9.irqmask);\n\t\t\tif ((dev9.irqmask != value) && ((dev9.irqmask | value) & dev9.irqcause))\n\t\t\t{\n\t\t\t\tDevCon.WriteLn(\"[SMAPIRQ] SPD_MASK scheduling IRQ while unmasking pending cause\");\n\t\t\t\tdev9Irq(1);\n\t\t\t}\n\t\t\tdev9.irqmask = value;\n\t\t\tDevCon.WriteLn(\"[SMAPIRQ] SPD_MASK after mask=%04x cause=%04x pending=%04x\",\n\t\t\t\tdev9.irqmask, dev9.irqcause, dev9.irqcause & dev9.irqmask);\n\t\t\treturn;\n""",
            "SpeedWrite interrupt registers",
        )

        smap = replace_once(
            smap,
            """\tfireIntR = true;\n\t//_DEV9irq(SMAP_INTR_RXEND,0);//now ? or when the fifo is full ? i guess now atm\n""",
            """\tDevCon.WriteLn(\"[SMAPIRQ] RX frame queued size=%d frame_count=%u cause=%04x mask=%04x\",\n\t\tpk->size, dev9Ru8(SMAP_R_RXFIFO_FRAME_CNT), dev9.irqcause, dev9.irqmask);\n\tfireIntR = true;\n\t//_DEV9irq(SMAP_INTR_RXEND,0);//now ? or when the fifo is full ? i guess now atm\n""",
            "rx_process",
        )

        smap = replace_once(
            smap,
            """\t\t\t\tif (test)\n\t\t\t\t{\n\t\t\t\t\tConsole.WriteLn(\"DEV9: Adapter Detection Hack - Resetting RX/TX\");\n\t\t\t\t\t_DEV9irq(SMAP_INTR_RXEND | SMAP_INTR_TXDNV, 100);\n\t\t\t\t}\n""",
            """\t\t\t\tif (test)\n\t\t\t\t{\n\t\t\t\t\tConsole.WriteLn(\"DEV9: Adapter Detection Hack - Resetting RX/TX\");\n\t\t\t\t\tDevCon.WriteLn(\"[SMAPIRQ] selftest PASS before inject cause=%04x mask=%04x pending=%04x\",\n\t\t\t\t\t\tdev9.irqcause, dev9.irqmask, dev9.irqcause & dev9.irqmask);\n\t\t\t\t\t_DEV9irq(SMAP_INTR_RXEND | SMAP_INTR_TXDNV, 100);\n\t\t\t\t\tDevCon.WriteLn(\"[SMAPIRQ] selftest after inject cause=%04x mask=%04x pending=%04x\",\n\t\t\t\t\t\tdev9.irqcause, dev9.irqmask, dev9.irqcause & dev9.irqmask);\n\t\t\t\t}\n""",
            "adapter self-test IRQ injection",
        )

        smap = replace_once(
            smap,
            """\t\tcase SMAP_R_INTR_CLR:\n\t\t\t//DevCon.WriteLn(\"DEV9: SMAP: SMAP_R_INTR_CLR 16bit write %x\", value);\n\t\t\tdev9.irqcause &= ~value;\n\t\t\treturn;\n""",
            """\t\tcase SMAP_R_INTR_CLR:\n\t\t{\n\t\t\tconst u16 old_cause = dev9.irqcause;\n\t\t\tdev9.irqcause &= ~value;\n\t\t\tDevCon.WriteLn(\"[SMAPIRQ] SMAP_INTR_CLR value=%04x old=%04x new=%04x mask=%04x pending=%04x\",\n\t\t\t\tvalue, old_cause, dev9.irqcause, dev9.irqmask, dev9.irqcause & dev9.irqmask);\n\t\t\treturn;\n\t\t}\n""",
            "SMAP_R_INTR_CLR",
        )

        smap = replace_once(
            smap,
            """\tif (fireIntR)\n\t{\n\t\tfireIntR = false;\n\t\t//Is this used to signal each individual packet, or just when there are packets in the RX fifo?\n\t\t//I think it just signals when there are packets in the RX fifo\n\t\t_DEV9irq(SMAP_INTR_RXEND, 0); //Make the call to _DEV9irq in a thread safe way\n\t}\n""",
            """\tif (fireIntR)\n\t{\n\t\tfireIntR = false;\n\t\tDevCon.WriteLn(\"[SMAPIRQ] smap_async raising RXEND cause=%04x mask=%04x\",\n\t\t\tdev9.irqcause, dev9.irqmask);\n\t\t//Is this used to signal each individual packet, or just when there are packets in the RX fifo?\n\t\t//I think it just signals when there are packets in the RX fifo\n\t\t_DEV9irq(SMAP_INTR_RXEND, 0); //Make the call to _DEV9irq in a thread safe way\n\t}\n""",
            "smap_async",
        )

        DEV9.write_text(dev9, encoding="utf-8")
        SMAP.write_text(smap, encoding="utf-8")
    except Exception:
        for path, backup in BACKUPS.items():
            if backup.exists():
                shutil.copy2(backup, path)
                backup.unlink()
        raise

    print(f"Applied focused SMAP IRQ probe to:\n  {DEV9}\n  {SMAP}")
    print("Backups created beside both source files.")
    print("This probe adds logging only and is not intended to change emulation behavior.")


if __name__ == "__main__":
    if len(sys.argv) > 2 or (len(sys.argv) == 2 and sys.argv[1] != "--restore"):
        print(f"Usage: {sys.argv[0]} [--restore]", file=sys.stderr)
        sys.exit(2)
    try:
        if len(sys.argv) == 2:
            restore()
        else:
            apply()
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        sys.exit(1)
