// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// The code has been designed for 64Mb flash and uses as file support the second memory card
#include <stdio.h>
//#include <winsock2.h>
#include "DEV9.h"
#include "Config.h"
#include "common/FileSystem.h"
#include "common/Path.h"

#include <algorithm>
#include <vector>

#define PAGE_SIZE_BITS 9
#define PAGE_SIZE (1 << PAGE_SIZE_BITS)
#define ECC_SIZE (16)
#define PAGE_SIZE_ECC (PAGE_SIZE + ECC_SIZE)
#define BLOCK_SIZE (16 * PAGE_SIZE)
#define BLOCK_SIZE_ECC (16 * PAGE_SIZE_ECC)
#define CARD_SIZE (1024 * BLOCK_SIZE)
#define CARD_SIZE_ECC (1024 * BLOCK_SIZE_ECC)


static u32 ctrl, cmd = static_cast<u32>(-1), address, id, counter, addrbyte;
static u8 data[PAGE_SIZE_ECC], file[CARD_SIZE_ECC];
static bool flash_file_loaded = false;
static bool warned_missing_flash_before_read = false;
static bool logged_biexec_lookup_transition = false;
static u32 flash_read_sequence = 0;

static std::string ResolveAbsolutePath(const std::string& path, const std::string& relative_to)
{
	if (Path::IsAbsolute(path))
		return Path::Canonicalize(path);

	return Path::Canonicalize(Path::Combine(relative_to, path));
}

static std::vector<std::string> GetFlashCandidatePaths()
{
	const std::string configured_path = EmuConfig.DEV9.FlashFile.empty() ? "flash.dat" : EmuConfig.DEV9.FlashFile;
	const std::string cwd = FileSystem::GetWorkingDirectory();
	std::vector<std::string> candidates;

	if (Path::IsAbsolute(configured_path))
	{
		candidates.push_back(Path::Canonicalize(configured_path));
	}
	else
	{
		candidates.push_back(ResolveAbsolutePath(configured_path, EmuFolders::Settings));
		if (configured_path != "flash.dat")
			candidates.push_back(ResolveAbsolutePath(configured_path, cwd));
		candidates.push_back(ResolveAbsolutePath("flash.dat", EmuFolders::Settings));
		candidates.push_back(ResolveAbsolutePath("flash.dat", cwd));
	}

	std::vector<std::string> unique;
	for (const std::string& candidate : candidates)
	{
		if (std::find(unique.begin(), unique.end(), candidate) == unique.end())
			unique.push_back(candidate);
	}
	return unique;
}

static std::string BuildCandidateListForLog(const std::vector<std::string>& candidates)
{
	std::string output;
	for (size_t i = 0; i < candidates.size(); i++)
	{
		if (!output.empty())
			output += "; ";
		output += candidates[i];
	}
	return output;
}

static void xfromman_call20_calculateXors(unsigned char buffer[128], unsigned char blah[4]);

static void calculateECC(u8 page[PAGE_SIZE_ECC])
{
	memset(page + PAGE_SIZE, 0x00, ECC_SIZE);
	xfromman_call20_calculateXors(page + 0 * (PAGE_SIZE >> 2), page + PAGE_SIZE + 0 * 3); //(ECC_SIZE>>2));
	xfromman_call20_calculateXors(page + 1 * (PAGE_SIZE >> 2), page + PAGE_SIZE + 1 * 3); //(ECC_SIZE>>2));
	xfromman_call20_calculateXors(page + 2 * (PAGE_SIZE >> 2), page + PAGE_SIZE + 2 * 3); //(ECC_SIZE>>2));
	xfromman_call20_calculateXors(page + 3 * (PAGE_SIZE >> 2), page + PAGE_SIZE + 3 * 3); //(ECC_SIZE>>2));
}

static const char* getCmdName(u32 cmd)
{
	switch (cmd)
	{
		case SM_CMD_READ1:
			return "READ1";
		case SM_CMD_READ2:
			return "READ2";
		case SM_CMD_READ3:
			return "READ3";
		case SM_CMD_RESET:
			return "RESET";
		case SM_CMD_WRITEDATA:
			return "WRITEDATA";
		case SM_CMD_PROGRAMPAGE:
			return "PROGRAMPAGE";
		case SM_CMD_ERASEBLOCK:
			return "ERASEBLOCK";
		case SM_CMD_ERASECONFIRM:
			return "ERASECONFIRM";
		case SM_CMD_GETSTATUS:
			return "GETSTATUS";
		case SM_CMD_READID:
			return "READID";
		default:
			return "unknown";
	}
}

void FLASHinit()
{
	id = FLASH_ID_64MBIT;
	counter = 0;
	addrbyte = 0;

	address = 0;
	memset(data, 0xFF, PAGE_SIZE);
	calculateECC(data);
	ctrl = FLASH_PP_READY;
	flash_file_loaded = false;
	warned_missing_flash_before_read = false;
	logged_biexec_lookup_transition = false;
	flash_read_sequence = 0;

	const std::vector<std::string> flash_candidates = GetFlashCandidatePaths();
	const std::string configured_flash = EmuConfig.DEV9.FlashFile.empty() ? "flash.dat" : EmuConfig.DEV9.FlashFile;
	DevCon.WriteLn("DEV9: flash image lookup (setting DEV9/Hdd/FlashFile='%s') candidates: %s",
		configured_flash.c_str(), BuildCandidateListForLog(flash_candidates).c_str());

	for (const std::string& flash_candidate : flash_candidates)
	{
		FILE* fd = fopen(flash_candidate.c_str(), "rb");
		if (fd == nullptr)
			continue;

		const size_t ret = fread(file, 1, CARD_SIZE_ECC, fd);
		fclose(fd);

		if (ret != CARD_SIZE_ECC)
		{
			DevCon.Warning("DEV9: flash image '%s' has wrong size (read=%zu expected=%u), ignoring.",
				flash_candidate.c_str(), ret, static_cast<unsigned>(CARD_SIZE_ECC));
			continue;
		}

		flash_file_loaded = true;
		DevCon.WriteLn("DEV9: loaded flash image from '%s'.", flash_candidate.c_str());
		break;
	}

	if (!flash_file_loaded)
	{
		DevCon.Warning(
			"DEV9: no flash backing file found. Place 'flash.dat' in the settings directory ('%s') or set DEV9/Hdd/FlashFile to an absolute or settings-relative path. Lookup attempted: %s. Using erased flash image (0xFF).",
			EmuFolders::Settings.c_str(), BuildCandidateListForLog(flash_candidates).c_str());
		memset(file, 0xFF, CARD_SIZE_ECC);
	}
}

static u32 decodeReadValue(const u8* src, int size)
{
	u32 value = 0;
	for (int i = 0; i < size; i++)
		value |= static_cast<u32>(src[i]) << (i * 8);
	return value;
}

static void logFlashReadWindow(const char* context)
{
	const u32 blocks = address / BLOCK_SIZE;
	const u32 block_offset = address - (blocks * BLOCK_SIZE);
	const u32 pages = block_offset / PAGE_SIZE;
	const u32 bytes = block_offset % PAGE_SIZE;
	DevCon.WriteLn(
		"DEV9: *FLASH %s seq=%u cmd=%s address=0x%08lX (block=%u page=%u byte=%u) counter=%u window=[%02X %02X %02X %02X]",
		context, flash_read_sequence, getCmdName(cmd), address, blocks, pages, bytes, counter,
		data[0], data[1], data[2], data[3]);
}

u32 FLASHread32(u32 addr, int size)
{
	u32 value, refill = 0;

	switch (addr)
	{
		case FLASH_R_DATA:
		{
			if (!flash_file_loaded && !warned_missing_flash_before_read)
			{
				DevCon.Warning("DEV9: *FLASH DATA read while flash image is missing; reads come from erased (0xFF) backing store.");
				warned_missing_flash_before_read = true;
			}

			value = decodeReadValue(&data[counter], size);
			counter += size;
			DevCon.WriteLn("DEV9: *FLASH DATA %dbit read 0x%08lX %s", size * 8, value, (ctrl & FLASH_PP_READ) ? "READ_ENABLE" : "READ_DISABLE");
			const u32 read_start = counter - static_cast<u32>(size);
			if (size == 4 && value == 0xFFFFFFFF && (data[read_start] != 0xFF || data[read_start + 1] != 0xFF || data[read_start + 2] != 0xFF || data[read_start + 3] != 0xFF))
			{
				DevCon.Warning("DEV9: *FLASH DATA suspicious 32-bit packing produced 0xFFFFFFFF but source bytes were %02X %02X %02X %02X",
					data[read_start], data[read_start + 1], data[read_start + 2], data[read_start + 3]);
			}
			if (cmd == SM_CMD_READ3)
			{
				if (counter >= PAGE_SIZE_ECC)
				{
					counter = PAGE_SIZE;
					refill = 1;
				}
			}
			else
			{
				if ((ctrl & FLASH_PP_NOECC) && (counter >= PAGE_SIZE))
				{
					counter %= PAGE_SIZE;
					refill = 1;
				}
				else if (!(ctrl & FLASH_PP_NOECC) && (counter >= PAGE_SIZE_ECC))
				{
					counter %= PAGE_SIZE_ECC;
					refill = 1;
				}
			}

			if (refill)
			{
				ctrl &= ~FLASH_PP_READY;
				address += PAGE_SIZE;
				address %= CARD_SIZE;
				memcpy(data, file + (address >> PAGE_SIZE_BITS) * PAGE_SIZE_ECC, PAGE_SIZE);
				calculateECC(data); // calculate ECC; should be in the file already
				ctrl |= FLASH_PP_READY;
				flash_read_sequence++;
				logFlashReadWindow("READ WINDOW REFILL");
			}

			return value;
		}

		case FLASH_R_CMD:
			DevCon.WriteLn("DEV9: *FLASH CMD %dbit read %s DENIED", size * 8, getCmdName(cmd));
			return cmd;

		case FLASH_R_ADDR:
			DevCon.WriteLn("DEV9: *FLASH ADDR %dbit read DENIED", size * 8);
			return 0;

		case FLASH_R_CTRL:
			DevCon.WriteLn("DEV9: *FLASH CTRL %dbit read 0x%08lX", size * 8, ctrl);
			return ctrl;

		case FLASH_R_ID:
			if (cmd == SM_CMD_READID)
			{
				DevCon.WriteLn("DEV9: *FLASH ID %dbit read 0x%08lX", size * 8, id);
				return id; //0x98=Toshiba/0xEC=Samsung maker code should be returned first
			}
			else if (cmd == SM_CMD_GETSTATUS)
			{
				value = 0x80 | ((ctrl & 1) << 6); // 0:0=pass, 6:ready/busy, 7:1=not protected
				DevCon.WriteLn("DEV9: *FLASH STATUS %dbit read 0x%08lX", size * 8, value);
				return value;
			} //else fall off
			return 0;

		default:
			DevCon.WriteLn("DEV9: *FLASH Unknown %dbit read at address %lx", size * 8, addr);
			return 0;
	}
}

void FLASHwrite32(u32 addr, u32 value, int size)
{

	switch (addr & 0x1FFFFFFF)
	{
		case FLASH_R_DATA:

			DevCon.WriteLn("DEV9: *FLASH DATA %dbit write 0x%08lX %s", size * 8, value, (ctrl & FLASH_PP_WRITE) ? "WRITE_ENABLE" : "WRITE_DISABLE");
			memcpy(&data[counter], &value, size);
			counter += size;
			counter %= PAGE_SIZE_ECC; //should not get past the last byte, but at the end
			break;

		case FLASH_R_CMD:
			if (!(ctrl & FLASH_PP_READY))
			{
				if ((value != SM_CMD_GETSTATUS) && (value != SM_CMD_RESET))
				{
					DevCon.WriteLn("DEV9: *FLASH CMD %dbit write %s ILLEGAL in busy mode - IGNORED", size * 8, getCmdName(value));
					break;
				}
			}
			if (cmd == SM_CMD_WRITEDATA)
			{
				if ((value != SM_CMD_PROGRAMPAGE) && (value != SM_CMD_RESET))
				{
					DevCon.WriteLn("DEV9: *FLASH CMD %dbit write %s ILLEGAL after WRITEDATA cmd - IGNORED", size * 8, getCmdName(value));
					ctrl &= ~FLASH_PP_READY; //go busy, reset is needed
					break;
				}
			}
			DevCon.WriteLn("DEV9: *FLASH CMD %dbit write %s", size * 8, getCmdName(value));
			if ((value == SM_CMD_READ1 || value == SM_CMD_READ2 || value == SM_CMD_READ3 || value == SM_CMD_READID) &&
				!(cmd == SM_CMD_READ1 || cmd == SM_CMD_READ2 || cmd == SM_CMD_READ3 || cmd == SM_CMD_READID))
			{
				logged_biexec_lookup_transition = false;
				DevCon.WriteLn("DEV9: *FLASH transition into read lookup (prev=%s -> next=%s)", getCmdName(cmd), getCmdName(value));
			}
			switch (value)
			{ // A8 bit is encoded in READ cmd;)
				case SM_CMD_READ1:
					counter = 0;
					if (cmd != SM_CMD_GETSTATUS)
						address = counter;
					addrbyte = 0;
					break;
				case SM_CMD_READ2:
					counter = PAGE_SIZE / 2;
					if (cmd != SM_CMD_GETSTATUS)
						address = counter;
					addrbyte = 0;
					break;
				case SM_CMD_READ3:
					counter = PAGE_SIZE;
					if (cmd != SM_CMD_GETSTATUS)
						address = counter;
					addrbyte = 0;
					break;
				case SM_CMD_RESET:
					FLASHinit();
					break;
				case SM_CMD_WRITEDATA:
					counter = 0;
					address = counter;
					addrbyte = 0;
					break;
				case SM_CMD_ERASEBLOCK:
					counter = 0;
					memset(data, 0xFF, PAGE_SIZE);
					address = counter;
					addrbyte = 1;
					break;
				case SM_CMD_PROGRAMPAGE: //fall
				case SM_CMD_ERASECONFIRM:
					ctrl &= ~FLASH_PP_READY;
					calculateECC(data);
					memcpy(file + (address / PAGE_SIZE) * PAGE_SIZE_ECC, data, PAGE_SIZE_ECC);
					/*write2file*/
					ctrl |= FLASH_PP_READY;
					break;
				case SM_CMD_GETSTATUS:
					break;
				case SM_CMD_READID:
					counter = 0;
					address = counter;
					addrbyte = 0;
					break;
				default:
					ctrl &= ~FLASH_PP_READY;
					return; //ignore any other command; go busy, reset is needed
			}
			cmd = value;
			break;

		case FLASH_R_ADDR:
			DevCon.WriteLn("DEV9: *FLASH ADDR %dbit write 0x%08lX", size * 8, value);
			address |= (value & 0xFF) << (addrbyte == 0 ? 0 : (1 + 8 * addrbyte));
			addrbyte++;
			DevCon.WriteLn("DEV9: *FLASH ADDR = 0x%08lX (addrbyte=%d)", address, addrbyte);
			if (!(value & 0x100))
			{ // address is complete
				if ((cmd == SM_CMD_READ1) || (cmd == SM_CMD_READ2) || (cmd == SM_CMD_READ3))
				{
					ctrl &= ~FLASH_PP_READY;
					memcpy(data, file + (address >> PAGE_SIZE_BITS) * PAGE_SIZE_ECC, PAGE_SIZE);
					calculateECC(data); // calculate ECC; should be in the file already
					ctrl |= FLASH_PP_READY;
					flash_read_sequence++;
					logFlashReadWindow("READ PAGE SELECT");
					if (!logged_biexec_lookup_transition)
					{
						const u32 blocks = address / BLOCK_SIZE;
						const u32 block_offset = address - (blocks * BLOCK_SIZE);
						const u32 pages = block_offset / PAGE_SIZE;
						DevCon.WriteLn(
							"DEV9: *FLASH BIEXEC lookup transition: flash_base=0x%08X selected_block=%u selected_page=%u address=0x%08lX",
							FLASH_REGBASE, blocks, pages, address);
						logged_biexec_lookup_transition = true;
					}
				}
				else if (cmd == SM_CMD_READID)
				{
					counter = (address & 1) ? 1 : 0;
					DevCon.WriteLn("DEV9: *FLASH READID pointer update address=0x%08lX -> counter=%u", address, counter);
				}
				addrbyte = 0; // address reset
				{
					const u32 blocks = address / BLOCK_SIZE;
					u32 pages = address - (blocks * BLOCK_SIZE);
					[[maybe_unused]]const u32 bytes = pages % PAGE_SIZE;
					pages = pages / PAGE_SIZE;
					DevCon.WriteLn("DEV9: *FLASH ADDR = 0x%08lX (%d:%d:%d) (addrbyte=%d) FINAL", address, blocks, pages, bytes, addrbyte);
				}
			}
			break;

		case FLASH_R_CTRL:
			DevCon.WriteLn("DEV9: *FLASH CTRL %dbit write 0x%08lX", size * 8, value);
			ctrl = (ctrl & FLASH_PP_READY) | (value & ~FLASH_PP_READY);
			break;

		case FLASH_R_ID:
			DevCon.WriteLn("DEV9: *FLASH ID %dbit write 0x%08lX DENIED :P", size * 8, value);
			break;

		default:
			DevCon.WriteLn("DEV9: *FLASH Unkwnown %dbit write at address 0x%08lX= 0x%08lX IGNORED", size * 8, addr, value);
			break;
	}
}

static unsigned char xor_table[256] = {
	0x00, 0x87, 0x96, 0x11, 0xA5, 0x22, 0x33, 0xB4, 0xB4, 0x33, 0x22, 0xA5, 0x11, 0x96, 0x87, 0x00,
	0xC3, 0x44, 0x55, 0xD2, 0x66, 0xE1, 0xF0, 0x77, 0x77, 0xF0, 0xE1, 0x66, 0xD2, 0x55, 0x44, 0xC3,
	0xD2, 0x55, 0x44, 0xC3, 0x77, 0xF0, 0xE1, 0x66, 0x66, 0xE1, 0xF0, 0x77, 0xC3, 0x44, 0x55, 0xD2,
	0x11, 0x96, 0x87, 0x00, 0xB4, 0x33, 0x22, 0xA5, 0xA5, 0x22, 0x33, 0xB4, 0x00, 0x87, 0x96, 0x11,
	0xE1, 0x66, 0x77, 0xF0, 0x44, 0xC3, 0xD2, 0x55, 0x55, 0xD2, 0xC3, 0x44, 0xF0, 0x77, 0x66, 0xE1,
	0x22, 0xA5, 0xB4, 0x33, 0x87, 0x00, 0x11, 0x96, 0x96, 0x11, 0x00, 0x87, 0x33, 0xB4, 0xA5, 0x22,
	0x33, 0xB4, 0xA5, 0x22, 0x96, 0x11, 0x00, 0x87, 0x87, 0x00, 0x11, 0x96, 0x22, 0xA5, 0xB4, 0x33,
	0xF0, 0x77, 0x66, 0xE1, 0x55, 0xD2, 0xC3, 0x44, 0x44, 0xC3, 0xD2, 0x55, 0xE1, 0x66, 0x77, 0xF0,
	0xF0, 0x77, 0x66, 0xE1, 0x55, 0xD2, 0xC3, 0x44, 0x44, 0xC3, 0xD2, 0x55, 0xE1, 0x66, 0x77, 0xF0,
	0x33, 0xB4, 0xA5, 0x22, 0x96, 0x11, 0x00, 0x87, 0x87, 0x00, 0x11, 0x96, 0x22, 0xA5, 0xB4, 0x33,
	0x22, 0xA5, 0xB4, 0x33, 0x87, 0x00, 0x11, 0x96, 0x96, 0x11, 0x00, 0x87, 0x33, 0xB4, 0xA5, 0x22,
	0xE1, 0x66, 0x77, 0xF0, 0x44, 0xC3, 0xD2, 0x55, 0x55, 0xD2, 0xC3, 0x44, 0xF0, 0x77, 0x66, 0xE1,
	0x11, 0x96, 0x87, 0x00, 0xB4, 0x33, 0x22, 0xA5, 0xA5, 0x22, 0x33, 0xB4, 0x00, 0x87, 0x96, 0x11,
	0xD2, 0x55, 0x44, 0xC3, 0x77, 0xF0, 0xE1, 0x66, 0x66, 0xE1, 0xF0, 0x77, 0xC3, 0x44, 0x55, 0xD2,
	0xC3, 0x44, 0x55, 0xD2, 0x66, 0xE1, 0xF0, 0x77, 0x77, 0xF0, 0xE1, 0x66, 0xD2, 0x55, 0x44, 0xC3,
	0x00, 0x87, 0x96, 0x11, 0xA5, 0x22, 0x33, 0xB4, 0xB4, 0x33, 0x22, 0xA5, 0x11, 0x96, 0x87, 0x00};

static void xfromman_call20_calculateXors(unsigned char buffer[128], unsigned char blah[4])
{
	unsigned char a = 0, b = 0, c = 0, i;

	for (i = 0; i < 128; i++)
	{
		a ^= xor_table[buffer[i]];
		if (xor_table[buffer[i]] & 0x80)
		{
			b ^= ~i;
			c ^= i;
		}
	}

	blah[0] = (~a) & 0x77;
	blah[1] = (~b) & 0x7F;
	blah[2] = (~c) & 0x7F;
}
