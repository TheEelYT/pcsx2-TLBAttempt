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
	const std::string eks_path = Path::Combine(directory, "eks.bin");
	const std::string cks_path = Path::Combine(directory, "cks.bin");
	const std::string kek_path = Path::Combine(directory, "kek.bin");
	const std::string civ_path = Path::Combine(directory, "civ.bin");

	const auto eks_blob = FileSystem::ReadBinaryFile(eks_path.c_str());
	const auto cks_blob = FileSystem::ReadBinaryFile(cks_path.c_str());
	const auto kek_blob = FileSystem::ReadBinaryFile(kek_path.c_str());
	const auto civ_blob = FileSystem::ReadBinaryFile(civ_path.c_str());

	if (!eks_blob || !cks_blob || !kek_blob || !civ_blob)
	{
		ERROR_LOG("MagicGate: failed to read {} legacy split blobs in '{}' (expected eks.bin/cks.bin/kek.bin/civ.bin)",
			from_override ? "override" : "fallback", directory);
		return false;
	}

	if (eks_blob->size() < 16 || cks_blob->size() < 16 || kek_blob->size() < 16 || civ_blob->size() < 9)
	{
		ERROR_LOG("MagicGate: legacy split blobs in '{}' have invalid sizes (eks={}, cks={}, kek={}, civ={})",
			directory, eks_blob->size(), cks_blob->size(), kek_blob->size(), civ_blob->size());
		return false;
	}

	// Legacy PR #4274-style files are treated as retail material for memory-card auth.
	MagicGateMaterial& retail = m_material[static_cast<size_t>(MagicGateKeyset::Retail)];
	std::memcpy(retail.key.data(), kek_blob->data(), 16);
	std::memcpy(retail.iv.data(), civ_blob->data(), 9);
	retail.valid = true;

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
	legacy_dirs.emplace_back(Path::Combine(std::string(Path::GetDirectory(BiosPath)), "magicgate"));
	legacy_dirs.emplace_back(Path::Combine(EmuFolders::DataRoot, "magicgate"));

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
