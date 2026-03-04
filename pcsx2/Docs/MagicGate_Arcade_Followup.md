# MagicGate Arcade Follow-Up (PR #4274)

This note records a targeted follow-up to behavior introduced during **PCSX2 PR #4274**, specifically preserving context from the **"PCSX2 PR #4274 reviewer note by balika011"**.

## Historical context

During review of PR #4274, it was reported that **Namco System 246/256** boot flow failed **before the decrypt stage**. This means we need observability at pre-decrypt milestones, not only at final KELF/content verification.

## Added pre-decrypt trace milestones

The current SIO/CDVD auth path should now be traced at these checkpoints:

1. **Card init** (`AUTH_F3` in SIO memcard auth path).
2. **`boot.bin` open** in the IOP host-file open path.
3. **Challenge exchange** result in CDVD mechacon auth (`0x86/0x87` response validation).
4. **Decrypt attempt entry** when KELF-header verification/publication path is entered (`0x88/0x8F`).

## Acceptance checkpoints

Use separate success criteria for arcade-mode progress versus retail PS2 memcard auth completeness.

### Arcade-mode checkpoints (Namco System 246/256 focused)

- A1. Card init milestone is emitted with selected keyset (typically arcade on COH-H BIOS).
- A2. `boot.bin` open milestone is emitted.
- A3. Challenge exchange milestone is reached (pass/fail both informative).
- A4. Decrypt-attempt milestone is reached (even if decrypt fails).

Arcade follow-up is considered **partially successful** when A1-A3 are reached and failures can be localized before decrypt.

### Retail PS2 memcard auth checkpoints

- R1. Standard memcard auth framing still completes (`AUTH_F3`, key select, crypt payload/response framing).
- R2. No regressions in non-arcade keyset selection behavior.
- R3. CDVD auth path reaches card verification and key publication where expected.

Retail auth remains the compatibility baseline, but arcade mode is tracked independently so partial forward progress can be measured.
