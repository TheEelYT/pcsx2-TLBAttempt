#!/usr/bin/env python3
from pathlib import Path
import shutil
import sys

ROOT = Path(__file__).resolve().parent
SOURCE = ROOT / "pcsx2" / "DEV9" / "DEV9.cpp"
BACKUP = Path(str(SOURCE) + ".pre-eeprom-probe")
MARKER = 'DEV9: EEPROM dump [%s]'


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{label}: expected exactly one match, found {count}")
    return text.replace(old, new, 1)


def restore() -> None:
    if not BACKUP.exists():
        raise RuntimeError(f"Backup not found: {BACKUP}")
    shutil.copy2(BACKUP, SOURCE)
    BACKUP.unlink()
    print(f"Restored {SOURCE}")


def apply_probe() -> None:
    if not SOURCE.exists():
        raise RuntimeError(f"Run this script from the repository root; missing {SOURCE}")

    text = SOURCE.read_text(encoding="utf-8")
    if MARKER in text:
        print("EEPROM probe is already applied.")
        return

    if BACKUP.exists():
        raise RuntimeError(
            f"Backup already exists: {BACKUP}\n"
            "Refusing to overwrite it. Restore it first with --restore if appropriate."
        )

    shutil.copy2(SOURCE, BACKUP)

    text = replace_once(
        text,
        "bool isRunning = false;\n",
        "bool isRunning = false;\n\n"
        "static void DumpEEPROM(const char* where)\n"
        "{\n"
        "\tConsole.WriteLn(\"DEV9: EEPROM dump [%s]\", where);\n"
        "\tfor (int i = 0; i < 32; i++)\n"
        "\t\tConsole.WriteLn(\"DEV9: EEPROM[%02d] = %04x\", i, dev9.eeprom[i]);\n"
        "}\n",
        "DumpEEPROM helper insertion",
    )

    text = replace_once(
        text,
        "#endif\n\n\tfor (int rxbi = 0; rxbi < (SMAP_BD_SIZE / 8); rxbi++)\n",
        "#endif\n\n\tDumpEEPROM(\"init\");\n\n"
        "\tfor (int rxbi = 0; rxbi < (SMAP_BD_SIZE / 8); rxbi++)\n",
        "init dump insertion",
    )

    text = replace_once(
        text,
        "void DEV9shutdown()\n"
        "{\n"
        "\tDevCon.WriteLn(\"DEV9: DEV9shutdown\");\n"
        "\tdelete dev9.ata;\n"
        "}\n",
        "void DEV9shutdown()\n"
        "{\n"
        "\tDumpEEPROM(\"shutdown\");\n"
        "\tDevCon.WriteLn(\"DEV9: DEV9shutdown\");\n"
        "\tdelete dev9.ata;\n"
        "}\n",
        "shutdown dump insertion",
    )

    text = replace_once(
        text,
        "\t\t\t\t\t\tif (dev9.eeprom_bit == 16)\n"
        "\t\t\t\t\t\t{\n"
        "\t\t\t\t\t\t\tdev9.eeprom_address++;\n"
        "\t\t\t\t\t\t\tdev9.eeprom_bit = 0;\n"
        "\t\t\t\t\t\t}\n",
        "\t\t\t\t\t\tif (dev9.eeprom_bit == 16)\n"
        "\t\t\t\t\t\t{\n"
        "\t\t\t\t\t\t\tConsole.WriteLn(\"DEV9: EEPROM write complete addr=%02x value=%04x\",\n"
        "\t\t\t\t\t\t\t\tdev9.eeprom_address, dev9.eeprom[dev9.eeprom_address]);\n"
        "\t\t\t\t\t\t\tdev9.eeprom_address++;\n"
        "\t\t\t\t\t\t\tdev9.eeprom_bit = 0;\n"
        "\t\t\t\t\t\t}\n",
        "EEPROM completed-write logging insertion",
    )

    SOURCE.write_text(text, encoding="utf-8")
    print(f"Applied EEPROM probe to {SOURCE}")
    print(f"Backup saved as {BACKUP}")
    print("This changes logging only; it does not intentionally change DEV9 behavior.")


if __name__ == "__main__":
    try:
        if len(sys.argv) == 2 and sys.argv[1] == "--restore":
            restore()
        elif len(sys.argv) == 1:
            apply_probe()
        else:
            print(f"Usage: {Path(sys.argv[0]).name} [--restore]", file=sys.stderr)
            sys.exit(2)
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        sys.exit(1)
