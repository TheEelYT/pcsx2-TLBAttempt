# DES backend (MagicGate)

This directory contains the scoped DES implementation used by PCSX2's internal
`MagicGateCrypto` wrapper for CDVD and memory-card authentication paths.

The implementation is adapted from the MIT-licensed `tarequeh/DES` project and
is used as the crypto backend for behavior originating from PCSX2 PR #4274.
