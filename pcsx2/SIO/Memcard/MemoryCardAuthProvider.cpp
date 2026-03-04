// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "SIO/Memcard/MemoryCardAuthProvider.h"

#include "Host.h"
#include "CDVD/CDVD.h"
#include "common/Console.h"
#include "common/Error.h"
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
	Error error;
	std::vector<u8> blob;
	if (!FileSystem::ReadBinaryFile(path.c_str(), &blob, &error))
	{
		ERROR_LOG("MagicGate: failed to read {} key blob '{}': {}",
			from_override ? "override" : "BIOS-coupled", path, error.GetDescription());
		return false;
	}

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
		std::vector<u8> iv_blob;
		if (FileSystem::ReadBinaryFile(iv_path.c_str(), &iv_blob, &error) && iv_blob.size() >= 9)
		{
			for (MagicGateMaterial& material : m_material)
				std::memcpy(material.iv.data(), iv_blob.data(), 9);
		}
		else
		{
			ERROR_LOG("MagicGate: failed to load IV override '{}': {}", iv_path,
				error.GetDescription());
		}
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

		WARNING_LOG("MagicGate: override blob failed, falling back to BIOS-coupled discovery.");
	}

	const std::string bios_blob_path = GetBiosCoupledBlobPath();
	if (!LoadBlob(bios_blob_path, false))
	{
		WARNING_LOG("MagicGate: no key material available for BIOS '{}', auth will run in degraded mode.",
			Path::GetFileName(BiosPath));
	}
}

MagicGateKeyset MemoryCardAuthProvider::GetDefaultKeyset() const
{
	return m_default_keyset;
}

const MagicGateMaterial& MemoryCardAuthProvider::GetMaterial(MagicGateKeyset keyset) const
{
	return m_material[static_cast<size_t>(keyset)];
}
