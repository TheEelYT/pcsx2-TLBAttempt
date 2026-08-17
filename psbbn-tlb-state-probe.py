#!/usr/bin/env python3
from pathlib import Path
import shutil
import sys

ROOT = Path(__file__).resolve().parent
COP0 = ROOT / "pcsx2" / "COP0.cpp"
BACKUP = ROOT / "pcsx2" / "COP0.cpp.pre-tlb-state-probe"
MARKER = "Temporary PSBBN TLB state probe"


def die(msg):
    print(f"ERROR: {msg}", file=sys.stderr)
    raise SystemExit(1)


def replace_once(text, old, new, label):
    n = text.count(old)
    if n != 1:
        die(f"expected exactly one {label} match, found {n}; source does not match psbbn-current-working")
    return text.replace(old, new, 1)


if "--restore" in sys.argv:
    if not BACKUP.exists():
        die(f"backup not found: {BACKUP}")
    shutil.copy2(BACKUP, COP0)
    BACKUP.unlink()
    print("Restored pcsx2/COP0.cpp and removed the TLB state probe backup.")
    raise SystemExit(0)

if not COP0.exists():
    die(f"missing {COP0}")

text = COP0.read_text(encoding="utf-8")
if MARKER in text:
    print("TLB state probe is already applied.")
    raise SystemExit(0)
if BACKUP.exists():
    die(f"backup already exists: {BACKUP}; refusing to overwrite it")

anchor = '''static bool tlbSlotIsInstalled(const tlbs& t)
{
\treturn t.isGlobal() || (t.EntryHi.ASID == tlbInstalledASID);
}
'''

helpers = anchor + '''
// Temporary PSBBN TLB state probe. Logging only; no emulation behavior changes.
static int psbbnTlbStateTraceCount = 0;
static int psbbnTlbCacheWarnCount = 0;
static int psbbnTlbAsidTraceCount = 0;

static bool psbbnTlbTouchesRegion(const tlbs& t)
{
\tconst u64 base = t.VPN2();
\tconst u64 pageSize = static_cast<u64>(t.Mask() + 1) << 12;
\tconst u64 end = base + pageSize * 2;
\treturn base < 0x0FC00000ULL && end > 0x0FB00000ULL;
}

static tlbs psbbnIncomingTlb()
{
\ttlbs t{};
\tt.PageMask.UL = cpuRegs.CP0.n.PageMask;
\tt.EntryHi.UL = cpuRegs.CP0.n.EntryHi;
\tt.EntryLo0.UL = cpuRegs.CP0.n.EntryLo0;
\tt.EntryLo1.UL = cpuRegs.CP0.n.EntryLo1;
\treturn t;
}

static bool psbbnAnyInterestingTlbSlot()
{
\tfor (int i = 0; i < 48; i++)
\t{
\t\tif (psbbnTlbTouchesRegion(tlb[i]))
\t\t\treturn true;
\t}
\treturn false;
}

static void psbbnCheckCachedTlbCount(const char* op, u32 index)
{
\tif (cachedTlbs.count < 44 || psbbnTlbCacheWarnCount >= 128)
\t\treturn;
\tconst int id = ++psbbnTlbCacheWarnCount;
\tConsole.Error(
\t\t"PSBBN TLBCACHE[%03d] op=%s idx=%u pc=%08X code=%08X cachedTlbs.count=%u",
\t\tid, op, index, cpuRegs.pc, cpuRegs.code, cachedTlbs.count);
}

static int psbbnTlbStateBegin(const char* op, u32 index, const tlbs& oldSlot)
{
\tconst tlbs incoming = psbbnIncomingTlb();
\tif ((!psbbnTlbTouchesRegion(oldSlot) && !psbbnTlbTouchesRegion(incoming)) ||
\t\tpsbbnTlbStateTraceCount >= 512)
\t\treturn 0;

\tconst int id = ++psbbnTlbStateTraceCount;
\tConsole.Error(
\t\t"PSBBN TLBSTATE[%03d] PRE op=%s idx=%u pc=%08X code=%08X installedASID=%02X cached=%u "
\t\t"oldVPN2=%08X oldASID=%02X oldLo0=%08X oldLo1=%08X oldResident=%u "
\t\t"inVPN2=%08X inASID=%02X PageMask=%08X inLo0=%08X inLo1=%08X V0=%u D0=%u V1=%u D1=%u",
\t\tid, op, index, cpuRegs.pc, cpuRegs.code, tlbInstalledASID & 0xff, cachedTlbs.count,
\t\toldSlot.VPN2(), oldSlot.EntryHi.ASID, oldSlot.EntryLo0.UL, oldSlot.EntryLo1.UL,
\t\ttlbSlotIsInstalled(oldSlot) ? 1u : 0u,
\t\tincoming.VPN2(), incoming.EntryHi.ASID, incoming.PageMask.UL,
\t\tincoming.EntryLo0.UL, incoming.EntryLo1.UL,
\t\tincoming.EntryLo0.V, incoming.EntryLo0.D, incoming.EntryLo1.V, incoming.EntryLo1.D);
\treturn id;
}

static void psbbnTlbStateEnd(int id, const char* op, u32 index, const tlbs& slot)
{
\tif (!id)
\t\treturn;
\tConsole.Error(
\t\t"PSBBN TLBSTATE[%03d] POST op=%s idx=%u installedASID=%02X cached=%u "
\t\t"newVPN2=%08X newASID=%02X newLo0=%08X newLo1=%08X V0=%u D0=%u V1=%u D1=%u resident=%u",
\t\tid, op, index, tlbInstalledASID & 0xff, cachedTlbs.count,
\t\tslot.VPN2(), slot.EntryHi.ASID, slot.EntryLo0.UL, slot.EntryLo1.UL,
\t\tslot.EntryLo0.V, slot.EntryLo0.D, slot.EntryLo1.V, slot.EntryLo1.D,
\t\ttlbSlotIsInstalled(slot) ? 1u : 0u);
}
'''
text = replace_once(text, anchor, helpers, "tlbSlotIsInstalled")

old = '''\t\tCOP0_LOG("COP0_TLBWI %d:%x,%x,%x,%x",
\t\t\tcpuRegs.CP0.n.Index, cpuRegs.CP0.n.PageMask, cpuRegs.CP0.n.EntryHi,
\t\t\tcpuRegs.CP0.n.EntryLo0, cpuRegs.CP0.n.EntryLo1);
\t\tUnmapTLB(tlb[j], j);
\t\tWriteTLB(j);
'''
new = '''\t\tCOP0_LOG("COP0_TLBWI %d:%x,%x,%x,%x",
\t\t\tcpuRegs.CP0.n.Index, cpuRegs.CP0.n.PageMask, cpuRegs.CP0.n.EntryHi,
\t\t\tcpuRegs.CP0.n.EntryLo0, cpuRegs.CP0.n.EntryLo1);
\t\tpsbbnCheckCachedTlbCount("TLBWI", j);
\t\tconst tlbs psbbnOldSlot = tlb[j];
\t\tconst int psbbnTraceId = psbbnTlbStateBegin("TLBWI", j, psbbnOldSlot);
\t\tUnmapTLB(tlb[j], j);
\t\tWriteTLB(j);
\t\tpsbbnTlbStateEnd(psbbnTraceId, "TLBWI", j, tlb[j]);
'''
text = replace_once(text, old, new, "TLBWI body")

old = '''\t\tCOP0_LOG("COP0_TLBWR %d:%x,%x,%x,%x\\n",
\t\t\tcpuRegs.CP0.n.Random, cpuRegs.CP0.n.PageMask, cpuRegs.CP0.n.EntryHi,
\t\t\tcpuRegs.CP0.n.EntryLo0, cpuRegs.CP0.n.EntryLo1);
\t\tUnmapTLB(tlb[j], j);
\t\tWriteTLB(j);
'''
new = '''\t\tCOP0_LOG("COP0_TLBWR %d:%x,%x,%x,%x\\n",
\t\t\tcpuRegs.CP0.n.Random, cpuRegs.CP0.n.PageMask, cpuRegs.CP0.n.EntryHi,
\t\t\tcpuRegs.CP0.n.EntryLo0, cpuRegs.CP0.n.EntryLo1);
\t\tpsbbnCheckCachedTlbCount("TLBWR", j);
\t\tconst tlbs psbbnOldSlot = tlb[j];
\t\tconst int psbbnTraceId = psbbnTlbStateBegin("TLBWR", j, psbbnOldSlot);
\t\tUnmapTLB(tlb[j], j);
\t\tWriteTLB(j);
\t\tpsbbnTlbStateEnd(psbbnTraceId, "TLBWR", j, tlb[j]);
'''
text = replace_once(text, old, new, "TLBWR body")

old = '''\t\t\t\tconst u32 newASID = cpuRegs.GPR.r[_Rt_].UL[0] & 0xff;

\t\t\t\tcpuRegs.CP0.n.EntryHi = cpuRegs.GPR.r[_Rt_].UL[0];

\t\t\t\tif (newASID != tlbInstalledASID)
'''
new = '''\t\t\t\tconst u32 newASID = cpuRegs.GPR.r[_Rt_].UL[0] & 0xff;

\t\t\t\tif (newASID != tlbInstalledASID && psbbnAnyInterestingTlbSlot() && psbbnTlbAsidTraceCount < 256)
\t\t\t\t{
\t\t\t\t\tconst int id = ++psbbnTlbAsidTraceCount;
\t\t\t\t\tConsole.Error(
\t\t\t\t\t\t"PSBBN TLBASID[%03d] pc=%08X code=%08X oldInstalled=%02X oldEntryHi=%08X newEntryHi=%08X newASID=%02X cached=%u",
\t\t\t\t\t\tid, cpuRegs.pc, cpuRegs.code, tlbInstalledASID & 0xff, cpuRegs.CP0.n.EntryHi,
\t\t\t\t\t\tcpuRegs.GPR.r[_Rt_].UL[0], newASID, cachedTlbs.count);
\t\t\t\t}

\t\t\t\tcpuRegs.CP0.n.EntryHi = cpuRegs.GPR.r[_Rt_].UL[0];

\t\t\t\tif (newASID != tlbInstalledASID)
'''
text = replace_once(text, old, new, "MTC0 EntryHi body")

shutil.copy2(COP0, BACKUP)
COP0.write_text(text, encoding="utf-8", newline="\n")
print("Applied PSBBN TLB state probe to pcsx2/COP0.cpp")
print(f"Backup: {BACKUP.relative_to(ROOT)}")
print("Restore with: python3 ./psbbn-tlb-state-probe.py --restore")
