// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include <array>
#include <optional>
#include <string>

#include "common/Pcsx2Defs.h"

enum class MagicGateKeyset : u8
{
	Retail = 0,
	Dev,
	Proto,
	Arcade,
};

struct MagicGateMaterial
{
	std::array<u8, 16> key = {};
	std::array<u8, 9> iv = {};
	bool valid = false;
};

class MemoryCardAuthProvider
{
public:
	void Refresh();
	MagicGateKeyset GetDefaultKeyset() const;
	const MagicGateMaterial& GetMaterial(MagicGateKeyset keyset) const;
	const char* GetKeysetName(MagicGateKeyset keyset) const;

private:
	std::array<MagicGateMaterial, 4> m_material = {};
	MagicGateKeyset m_default_keyset = MagicGateKeyset::Retail;

	std::optional<std::string> LoadBlobPathFromSettings() const;
	std::string GetBiosCoupledBlobPath() const;
	bool LoadBlob(const std::string& path, bool from_override);
	bool LoadLegacySplitBlobs(const std::string& directory, bool from_override);
	void DetermineDefaultKeyset();
};
