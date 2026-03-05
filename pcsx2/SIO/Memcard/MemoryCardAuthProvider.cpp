// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "SIO/Memcard/MemoryCardAuthProvider.h"

#include "Host.h"
#include "Config.h"
#include "CDVD/CDVD.h"
#include "common/Console.h"
#include "common/FileSystem.h"
#include "common/Path.h"
#include "ps2/BiosTools.h"

#include <cstring>
#include <vector>

static constexpr const char* SETTINGS_SECTION = "MemoryCards";
static constexpr const char* SETTINGS_BLOB_PATH = "MagicGateKeyBlobPath";
static constexpr const char* SETTINGS_IV_BLOB_PATH = "MagicGateIVBlobPath";

const char* MemoryCardAuthProvider::GetKeysetName(MagicGateKeyset keyset) const
{
	switch (keyset)
	{
		case MagicGateKeyset::Retail:
			return "retail";
		case MagicGateKeyset::Dev:
			return "dev";
		case MagicGateKeyset::Proto:
			return "proto";
		case MagicGateKeyset::Arcade:
			return "arcade";
		default:
			return "unknown";
	}
}

std::optional<std::string> MemoryCardAuthProvider::LoadBlobPathFromSettings() const
{
	const std::string configured_path = Host::GetStringSettingValue(SETTINGS_SECTION, SETTINGS_BLOB_PATH, "");
	if (configured_path.empty())
		return std::nullopt;

	return Path::Canonicalize(configured_path);
}

std::string MemoryCardAuthProvider::GetBiosCoupledBlobPath() const
{
	// Modernized replacement for PR #4274's eks.bin/cks.bin/kek.bin/civ.bin approach:
	// key material follows the active BIOS base path, similar to NVRAM/MEC sidecar files.
	return cdvdGetBiosSidecarPath("mgk");
}

void MemoryCardAuthProvider::DetermineDefaultKeyset()
{
	const bool is_devel = (BiosDescription.find("Devel") != std::string::npos);
	const bool is_arcade = (BiosZone == "COH-H");
	const bool is_proto = (BiosZone == "Test" || BiosRegion == 9 || BiosVersion < 0x0100);

	if (is_arcade)
		m_default_keyset = MagicGateKeyset::Arcade;
	else if (is_devel || BiosZone == "T10K")
		m_default_keyset = MagicGateKeyset::Dev;
	else if (is_proto)
		m_default_keyset = MagicGateKeyset::Proto;
	else
		m_default_keyset = MagicGateKeyset::Retail;
}

bool MemoryCardAuthProvider::LoadBlob(const std::string& path, bool from_override)
{
	auto blob_opt = FileSystem::ReadBinaryFile(path.c_str());
	if (!blob_opt.has_value())
	{
		ERROR_LOG("MagicGate: failed to read {} key blob '{}'",
			from_override ? "override" : "BIOS-coupled", path);
		return false;
	}
	const std::vector<u8>& blob = *blob_opt;

	if (blob.size() < 73)
	{
		ERROR_LOG("MagicGate: key blob '{}' is {} bytes, expected at least 73 bytes", path, blob.size());
		return false;
	}

	for (size_t i = 0; i < m_material.size(); i++)
	{
		std::memcpy(m_material[i].key.data(), blob.data() + (i * 16), 16);
		m_material[i].valid = true;
	}

	if (blob.size() >= 100)
	{
		for (size_t i = 0; i < m_material.size(); i++)
			std::memcpy(m_material[i].iv.data(), blob.data() + 64 + (i * 9), 9);
	}
	else
	{
		for (MagicGateMaterial& material : m_material)
			std::memcpy(material.iv.data(), blob.data() + 64, 9);
	}

	const std::string iv_path = Host::GetStringSettingValue(SETTINGS_SECTION, SETTINGS_IV_BLOB_PATH, "");
	if (!iv_path.empty())
	{
		auto iv_blob_opt = FileSystem::ReadBinaryFile(iv_path.c_str());
		if (iv_blob_opt && iv_blob_opt->size() >= 9)
		{
			for (MagicGateMaterial& material : m_material)
				std::memcpy(material.iv.data(), iv_blob_opt->data(), 9);
		}
		else
		{
			ERROR_LOG("MagicGate: failed to load IV override '{}'", iv_path);
		}
	}

	return true;
}


bool MemoryCardAuthProvider::LoadLegacySplitBlobs(const std::string& directory, bool from_override)
{
	const std::string cks_path = Path::Combine(directory, "cks.bin");
	const auto cks_blob = FileSystem::ReadBinaryFile(cks_path.c_str());
	if (!cks_blob)
	{
		ERROR_LOG("MagicGate: failed to read {} legacy split blob '{}'", from_override ? "override" : "fallback", cks_path);
		return false;
	}

	// Legacy PR #4274 layout for memory-card material is carried by cks.bin.
	// Common layout seen in the wild is 96 bytes: 4x16-byte keys + 4x8-byte IV blocks.
	if (cks_blob->size() < 96)
	{
		ERROR_LOG("MagicGate: legacy cks.bin '{}' has invalid size {} (expected at least 96 bytes)", cks_path, cks_blob->size());
		return false;
	}

	for (size_t i = 0; i < m_material.size(); i++)
	{
		MagicGateMaterial& material = m_material[i];
		std::memcpy(material.key.data(), cks_blob->data() + (i * 16), 16);

		if (cks_blob->size() >= 100)
		{
			std::memcpy(material.iv.data(), cks_blob->data() + 64 + (i * 9), 9);
		}
		else
		{
			std::memcpy(material.iv.data(), cks_blob->data() + 64 + (i * 8), 8);
			material.iv[8] = 0x00;
		}

		material.valid = true;
	}

	return true;
}
void MemoryCardAuthProvider::Refresh()
{
	for (MagicGateMaterial& material : m_material)
		material = {};

	DetermineDefaultKeyset();

	if (const std::optional<std::string> override_path = LoadBlobPathFromSettings(); override_path.has_value())
	{
		if (LoadBlob(*override_path, true))
		{
			INFO_LOG("MagicGate: using override blob '{}'", *override_path);
			return;
		}

		if (FileSystem::DirectoryExists(override_path->c_str()) && LoadLegacySplitBlobs(*override_path, true))
		{
			INFO_LOG("MagicGate: using override legacy split key files from '{}'", *override_path);
			return;
		}

		WARNING_LOG("MagicGate: override blob failed, falling back to BIOS-coupled discovery.");
	}

	const std::string bios_blob_path = GetBiosCoupledBlobPath();
	if (LoadBlob(bios_blob_path, false))
		return;

	std::vector<std::string> legacy_dirs;
	legacy_dirs.emplace_back(EmuFolders::MagicGate);
	legacy_dirs.emplace_back(Path::Combine(std::string(Path::GetDirectory(BiosPath)), "magicgate"));

	for (const std::string& dir : legacy_dirs)
	{
		if (!FileSystem::DirectoryExists(dir.c_str()))
			continue;

		if (LoadLegacySplitBlobs(dir, false))
		{
			INFO_LOG("MagicGate: using legacy split key files from '{}'", dir);
			return;
		}
	}

	WARNING_LOG("MagicGate: no key material available for BIOS '{}', auth will run in degraded mode.",
		Path::GetFileName(BiosPath));
}

MagicGateKeyset MemoryCardAuthProvider::GetDefaultKeyset() const
{
	return m_default_keyset;
}

const MagicGateMaterial& MemoryCardAuthProvider::GetMaterial(MagicGateKeyset keyset) const
{
	return m_material[static_cast<size_t>(keyset)];
}
