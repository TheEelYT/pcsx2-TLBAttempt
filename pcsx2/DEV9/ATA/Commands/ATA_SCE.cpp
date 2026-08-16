// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "DEV9/ATA/ATA.h"
#include "DEV9/DEV9.h"

void ATA::HDD_SCE()
{
	const char* command_name = "UNKNOWN";
	switch (regFeature)
	{
		case 0xF1:
			command_name = "SECURITY_SET_PASSWORD";
			break;
		case 0xF2:
			command_name = "SECURITY_UNLOCK";
			break;
		case 0xF3:
			command_name = "SECURITY_ERASE_PREPARE";
			break;
		case 0xF4:
			command_name = "SECURITY_ERASE_UNIT";
			break;
		case 0xF5:
			command_name = "SECURITY_FREEZE_LOCK";
			break;
		case 0x20:
			command_name = "SECURITY_READ_ID";
			break;
		case 0x30:
			command_name = "SECURITY_WRITE_ID";
			break;
		case 0xEC:
			command_name = "IDENTIFY_DRIVE";
			break;
	}

	Console.WriteLn(
		"DEV9: PSBBN security probe: SCE %s (feature=0x%02X) nsector=0x%02X sector=0x%02X "
		"lcyl=0x%02X hcyl=0x%02X select=0x%02X status=0x%02X",
		command_name, regFeature, regNsector, regSector, regLcyl, regHcyl, regSelect, regStatus);

	switch (regFeature)
	{
		case 0xF1: // ATA_SCE_SECURITY_SET_PASSWORD
		case 0xF2: // ATA_SCE_SECURITY_UNLOCK
		case 0xF3: // ATA_SCE_SECURITY_ERASE_PREPARE
		case 0xF4: // ATA_SCE_SECURITY_ERASE_UNIT
		case 0xF5: // ATA_SCE_SECURITY_FREEZE_LOCK
		case 0x20: // ATA_SCE_SECURITY_READ_ID
		case 0x30: // ATA_SCE_SECURITY_WRITE_ID
			Console.Error("DEV9: ATA: SCE command %s (0x%02X) not implemented", command_name, regFeature);
			CmdNoDataAbort();
			break;
		case 0xEC:
			SCE_IDENTIFY_DRIVE();
			break;
		default:
			Console.Error("DEV9: ATA: Unknown SCE command %x", regFeature);
			CmdNoDataAbort();
			return;
	}
}
// All games that have ability to install data into HDD will verify HDD by checking that this command completes successfully. Resident Evil: Outbreak for example
// Only a few games/apps make use of the returned data, see Final Fantasy XI or the HDD Utility disks, neither of which work yet
// Also PSX DESR bioses use this response for HDD encryption and decryption.
// Use of external HDD ID (not implemented) file may be necessary for users with protected titles installed to the SCE HDD and then dumped.
// For example: PS2 BB Navigator, PlayOnline Viewer, Bomberman Online, Nobunaga No Yabou Online, Pop'n Taisen Puzzle-Dama Online

// PS2 ID Dumper can be used as test case
void ATA::SCE_IDENTIFY_DRIVE()
{
	Console.WriteLn("DEV9: PSBBN security probe: returning current 512-byte SCE identify buffer");

	PreCmd();

	pioDRQEndTransferFunc = nullptr;
	DRQCmdPIODataToHost(sceSec, 256 * 2, 0, 256 * 2, true);
}
