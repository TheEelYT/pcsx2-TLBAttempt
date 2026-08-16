// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+
//
// Experimental MagicGate KELF compatibility work based directly on balika011's
// PCSX2 PR #4274.  This intentionally keeps Sony key material external.
#pragma once

#include "common/Console.h"
#include "common/FileSystem.h"
#include "common/Path.h"
#include "ps2/BiosTools.h"

#include <array>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
#include "common/RedtapeWindows.h"
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")
#endif

namespace MagicGateKelf
{
#pragma pack(push, 1)
struct KeyStore
{
	u8 CardKeyLow[3][8];
	u8 CardKeyHi[3][8];
	u8 CardKey2Low[3][8];
	u8 CardKey2Hi[3][8];
	u8 CardIV[3][8];
	u8 CardIV2[3][8];
	u8 KbitMasterKey[16];
	u8 KcMasterKey[16];
	u8 KbitIv[8];
	u8 KcIv[8];
	u8 icvps2LowKey[16];
	u8 icvps2HiKey[16];
	u8 icvps2LowIV[8];
	u8 icvps2HiIV[8];
	u8 SignatureMasterKey[8];
	u8 SignatureHashKey[8];
	u8 RootSigHashKey[16];
	u8 RootSigMasterKey[8];
	u8 ContentIV[8];
	u8 ContentTableIV[8];
	u8 ChallengeIV[8];
};
#pragma pack(pop)

static_assert(sizeof(KeyStore) == 304);

static constexpr std::array<u16, 72> s_memory_card_key_indexes = {
	0x0018, 0xFFFF, 0xFFFF, 0x001C, 0xFFFF, 0xFFFF, 0x0020, 0xFFFF, 0xFFFF, 0x0024, 0xFFFF, 0xFFFF, 0x0028, 0xFFFF, 0xFFFF, 0x002C, 0xFFFF, 0xFFFF,
	0x0030, 0x0048, 0x0060, 0x0034, 0x004C, 0x0064, 0x0038, 0x0050, 0x0068, 0x003C, 0x0054, 0x006C, 0x0040, 0x0058, 0x0070, 0x0044, 0x005C, 0x0074,
	0x0000, 0x1000, 0x1001, 0x0004, 0x1002, 0x1003, 0x0008, 0x1004, 0x1005, 0x000C, 0x1006, 0x1007, 0x0010, 0x1008, 0x1009, 0x0014, 0x100A, 0x100B,
	0x0090, 0x00A8, 0x00A8, 0x0094, 0x00AC, 0x00AC, 0x0098, 0x00B0, 0x00B0, 0x009C, 0x00B4, 0x00B4, 0x00A0, 0x00B8, 0x00B8, 0x00A4, 0x00BC, 0x00BC,
};
static constexpr std::array<u16, 4> s_kelf_keys_index = {0x110, 0x110, 0x00C4, 0x015C};

// kelftool's MBR user-defined header. Restricting the first experiment to this
// format avoids changing MagicGate behavior for unrelated software.
static constexpr std::array<u8, 16> s_mbr_user_header = {
	0x01, 0x00, 0x00, 0x04, 0x00, 0x02, 0x01, 0x57,
	0x07, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x2A,
};

inline KeyStore s_key_store = {};
inline std::array<u8, 1024> s_eks = {};
inline std::array<u8, 96> s_cks = {};
inline std::array<u8, 16> s_kek = {};
inline std::array<u8, 8> s_civ = {};
inline bool s_key_material_loaded = false;
inline bool s_key_load_attempted = false;
inline std::string s_key_bios_path;
inline bool s_header_decrypted = false;
inline bool s_header_attempted = false;
inline bool s_content_stage_logged = false;
inline bool s_transform_in_progress = false;

static u16 ReadLE16(const u8* ptr)
{
	return static_cast<u16>(ptr[0] | (static_cast<u16>(ptr[1]) << 8));
}

static u32 ReadLE32(const u8* ptr)
{
	return static_cast<u32>(ptr[0]) |
		(static_cast<u32>(ptr[1]) << 8) |
		(static_cast<u32>(ptr[2]) << 16) |
		(static_cast<u32>(ptr[3]) << 24);
}

template <size_t N>
static u16 ReadWord(const std::array<u8, N>& blob, size_t word_index)
{
	const size_t offset = word_index * 2;
	return (offset + 1 < blob.size()) ? ReadLE16(&blob[offset]) : 0;
}

static void StoreLE16(u8* dst, u16 value)
{
	dst[0] = static_cast<u8>(value);
	dst[1] = static_cast<u8>(value >> 8);
}

static void Xor8(const u8* input, u8* in_out)
{
	for (size_t i = 0; i < 8; i++)
		in_out[i] ^= input[i];
}

#ifdef _WIN32
static bool DesCrypt(const u8* key, const u8* input, u8* output, bool decrypt)
{
	BCRYPT_ALG_HANDLE algorithm = nullptr;
	BCRYPT_KEY_HANDLE key_handle = nullptr;
	ULONG object_size = 0;
	ULONG result_size = 0;
	bool success = false;

	NTSTATUS status = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_DES_ALGORITHM, nullptr, 0);
	if (status < 0)
		goto cleanup;

	status = BCryptSetProperty(algorithm, BCRYPT_CHAINING_MODE,
		reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_ECB)),
		static_cast<ULONG>(sizeof(BCRYPT_CHAIN_MODE_ECB)), 0);
	if (status < 0)
		goto cleanup;

	status = BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
		reinterpret_cast<PUCHAR>(&object_size), sizeof(object_size), &result_size, 0);
	if (status < 0 || object_size == 0)
		goto cleanup;

	{
		std::vector<u8> key_object(object_size);
		status = BCryptGenerateSymmetricKey(algorithm, &key_handle, key_object.data(), object_size,
			reinterpret_cast<PUCHAR>(const_cast<u8*>(key)), 8, 0);
		if (status < 0)
			goto cleanup;

		std::array<u8, 8> input_copy = {};
		std::memcpy(input_copy.data(), input, input_copy.size());
		ULONG bytes_written = 0;
		if (decrypt)
		{
			status = BCryptDecrypt(key_handle, input_copy.data(), static_cast<ULONG>(input_copy.size()),
				nullptr, nullptr, 0, output, 8, &bytes_written, 0);
		}
		else
		{
			status = BCryptEncrypt(key_handle, input_copy.data(), static_cast<ULONG>(input_copy.size()),
				nullptr, nullptr, 0, output, 8, &bytes_written, 0);
		}
		success = (status >= 0 && bytes_written == 8);
	}

cleanup:
	if (key_handle)
		BCryptDestroyKey(key_handle);
	if (algorithm)
		BCryptCloseAlgorithmProvider(algorithm, 0);
	return success;
}
#else
static bool DesCrypt(const u8*, const u8*, u8*, bool)
{
	return false;
}
#endif

static bool DesEncrypt(const u8* key, u8* block)
{
	std::array<u8, 8> output = {};
	if (!DesCrypt(key, block, output.data(), false))
		return false;
	std::memcpy(block, output.data(), output.size());
	return true;
}

static bool DesDecrypt(const u8* key, u8* block)
{
	std::array<u8, 8> output = {};
	if (!DesCrypt(key, block, output.data(), true))
		return false;
	std::memcpy(block, output.data(), output.size());
	return true;
}

static bool DoubleDesEncrypt(const u8* key, u8* block)
{
	return DesEncrypt(key, block) && DesDecrypt(key + 8, block) && DesEncrypt(key, block);
}

static bool DoubleDesDecrypt(const u8* key, u8* block)
{
	return DesDecrypt(key, block) && DesEncrypt(key + 8, block) && DesDecrypt(key, block);
}

template <size_t N>
static bool ReadExactBlob(const std::string& directory, const char* filename, std::array<u8, N>* destination)
{
	const std::string path = Path::Combine(directory, filename);
	const std::optional<std::vector<u8>> contents = FileSystem::ReadBinaryFile(path.c_str());
	if (!contents.has_value())
	{
		Console.Error("[MG] PSBBN: could not read %s next to the active BIOS", filename);
		return false;
	}
	if (contents->size() != N)
	{
		Console.Error("[MG] PSBBN: %s has size %zu, expected %zu bytes", filename, contents->size(), N);
		return false;
	}

	std::memcpy(destination->data(), contents->data(), N);
	return true;
}

static bool ReconstructRetailKeyStore()
{
	std::memset(&s_key_store, 0, sizeof(s_key_store));
	u8* const key_store_bytes = reinterpret_cast<u8*>(&s_key_store);
	size_t output_word = 0;

	auto append_word = [&](u16 value) {
		if ((output_word + 1) * 2 > sizeof(s_key_store))
			return false;
		StoreLE16(&key_store_bytes[output_word * 2], value);
		output_word++;
		return true;
	};

	// Retail is index set 1 in balika011's original implementation.
	constexpr size_t index_set = 1;
	for (size_t i = 0; i < 18; i++)
	{
		const u16 key_index = s_memory_card_key_indexes[index_set * 18 + i];
		if (key_index >= 0x200)
		{
			if (key_index == 0xFFFF)
			{
				for (size_t j = 0; j < 4; j++)
					if (!append_word(0))
						return false;
			}
			else
			{
				const size_t base = static_cast<u8>(key_index) * 4;
				if (base + 3 >= (s_cks.size() / 2))
					return false;
				for (size_t j = 0; j < 4; j++)
					if (!append_word(ReadWord(s_cks, base + j)))
						return false;
			}
		}
		else
		{
			if (static_cast<size_t>(key_index) + 3 >= (s_eks.size() / 2))
				return false;
			for (size_t j = 0; j < 4; j++)
				if (!append_word(ReadWord(s_eks, static_cast<size_t>(key_index) + j)))
					return false;
		}
	}

	const size_t kelf_base = s_kelf_keys_index[index_set];
	if (kelf_base + (19 * 4) > (s_eks.size() / 2))
		return false;
	for (size_t i = 0; i < 19 * 4; i++)
		if (!append_word(ReadWord(s_eks, kelf_base + i)))
			return false;

	constexpr size_t tail_base = 192;
	for (size_t i = 0; i < 4; i++)
		if (!append_word(ReadWord(s_eks, tail_base + i)))
			return false;

	if (output_word * 2 != sizeof(s_key_store))
		return false;

	for (size_t i = 0; i < sizeof(s_key_store); i += 8)
	{
		if (!DoubleDesDecrypt(s_kek.data(), &key_store_bytes[i]))
			return false;
	}
	return true;
}

static bool EnsureKeyMaterial()
{
	if (s_key_bios_path != BiosPath)
	{
		s_key_bios_path = BiosPath;
		s_key_material_loaded = false;
		s_key_load_attempted = false;
	}
	if (s_key_material_loaded)
		return true;
	if (s_key_load_attempted)
		return false;

	s_key_load_attempted = true;
	const std::string directory(Path::GetDirectory(BiosPath));
	if (!ReadExactBlob(directory, "eks.bin", &s_eks) ||
		!ReadExactBlob(directory, "cks.bin", &s_cks) ||
		!ReadExactBlob(directory, "kek.bin", &s_kek) ||
		!ReadExactBlob(directory, "civ.bin", &s_civ))
	{
		return false;
	}

#ifdef _WIN32
	if (!ReconstructRetailKeyStore())
	{
		Console.Error("[MG] PSBBN: failed to reconstruct the retail MagicGate keystore");
		return false;
	}
#else
	Console.Error("[MG] PSBBN: this experimental KELF decrypt path currently requires Windows CNG");
	return false;
#endif

	s_key_material_loaded = true;
	Console.WriteLn("[MG] PSBBN: loaded external retail MagicGate key material");
	return true;
}

static bool DecryptAndVerifyMbrHeader()
{
	const size_t data_size = static_cast<size_t>(cdvd.mg_size);
	if (data_size < 0x20 || data_size > sizeof(cdvd.mg_buffer))
		return false;

	u8* const original = cdvd.mg_buffer.data;
	if (std::memcmp(original, s_mbr_user_header.data(), s_mbr_user_header.size()) != 0)
		return false;

	if (!EnsureKeyMaterial())
		return false;

	// Work on a copy so a failed authentication never leaves the emulated buffer
	// half decrypted. Needs proper testing against more than the OSDMenu MBR KELF.
	std::vector<u8> work(original, original + data_size);
	const u16 declared_header_size = ReadLE16(&work[0x14]);
	const u16 flags = ReadLE16(&work[0x18]);
	const u16 ban_count = ReadLE16(&work[0x1A]);
	if (declared_header_size != data_size)
	{
		Console.Error("[MG] PSBBN: KELF header size mismatch (declared=0x%X received=0x%zX)", declared_header_size, data_size);
		return false;
	}

	size_t signed_header_size = 0x20 + (static_cast<size_t>(ban_count) * 0x10);
	if (signed_header_size > data_size)
		return false;
	if (flags & 1)
	{
		if (signed_header_size >= data_size)
			return false;
		signed_header_size += static_cast<size_t>(work[signed_header_size]) + 1;
	}
	if (signed_header_size + 8 + 16 + 16 > data_size)
		return false;

	std::array<u8, 8> header_signature = {};
	for (size_t i = 0; i < (signed_header_size & ~static_cast<size_t>(7)); i += 8)
	{
		Xor8(&work[i], header_signature.data());
		if (!DesEncrypt(s_key_store.SignatureMasterKey, header_signature.data()))
			return false;
	}
	if (!DesDecrypt(s_key_store.SignatureHashKey, header_signature.data()) ||
		!DesEncrypt(s_key_store.SignatureMasterKey, header_signature.data()))
	{
		return false;
	}
	if (std::memcmp(header_signature.data(), &work[signed_header_size], 8) != 0)
	{
		Console.Error("[MG] PSBBN: invalid KELF header signature");
		return false;
	}

	size_t offset = signed_header_size + 8;
	std::array<u8, 8> nonce = {};
	for (size_t i = 0; i < 8; i++)
		nonce[i] = work[i] ^ work[i + 8];

	std::array<u8, 16> kek = {};
	for (size_t i = 0; i < 8; i++)
	{
		kek[i] = s_key_store.KbitIv[i] ^ nonce[i];
		kek[i + 8] = s_key_store.KcIv[i] ^ nonce[i];
	}
	if (!DoubleDesEncrypt(s_key_store.KbitMasterKey, kek.data()) ||
		!DoubleDesEncrypt(s_key_store.KcMasterKey, kek.data() + 8))
	{
		return false;
	}

	std::array<u8, 16> kbit = {};
	std::array<u8, 16> kc = {};
	std::memcpy(kbit.data(), &work[offset], 16);
	offset += 16;
	std::memcpy(kc.data(), &work[offset], 16);
	offset += 16;
	if (!DoubleDesDecrypt(kek.data(), kbit.data()) ||
		!DoubleDesDecrypt(kek.data(), kbit.data() + 8) ||
		!DoubleDesDecrypt(kek.data(), kc.data()) ||
		!DoubleDesDecrypt(kek.data(), kc.data() + 8))
	{
		return false;
	}

	const size_t kbit_offset = offset - 32;
	const size_t kc_offset = offset - 16;
	std::memcpy(&work[kbit_offset], kbit.data(), kbit.size());
	std::memcpy(&work[kc_offset], kc.data(), kc.size());

	const size_t bit_offset = offset;
	if (bit_offset + 8 + 16 > data_size)
		return false;

	std::array<u8, 8> previous_ciphertext = {};
	std::memcpy(previous_ciphertext.data(), &work[bit_offset], 8);
	if (!DoubleDesDecrypt(kbit.data(), &work[bit_offset]))
		return false;
	Xor8(s_key_store.ContentTableIV, &work[bit_offset]);

	const u8 block_count = work[bit_offset + 4];
	if (block_count == 0 || block_count > 64)
	{
		Console.Error("[MG] PSBBN: invalid KELF BIT block count %u", static_cast<unsigned int>(block_count));
		return false;
	}
	const size_t bit_length = 8 + (static_cast<size_t>(block_count) * 16);
	if (bit_offset + bit_length + 16 > data_size)
		return false;

	for (size_t pos = 8; pos < bit_length; pos += 8)
	{
		std::array<u8, 8> ciphertext = {};
		std::memcpy(ciphertext.data(), &work[bit_offset + pos], 8);
		if (!DoubleDesDecrypt(kbit.data(), &work[bit_offset + pos]))
			return false;
		Xor8(previous_ciphertext.data(), &work[bit_offset + pos]);
		previous_ciphertext = ciphertext;
	}

	if (ReadLE32(&work[bit_offset]) != data_size ||
		work[bit_offset + 5] != 0 || work[bit_offset + 6] != 0 || work[bit_offset + 7] != 0)
	{
		Console.Error("[MG] PSBBN: decrypted KELF BIT header is invalid");
		return false;
	}

	unsigned int signed_blocks = 0;
	for (u8 i = 0; i < block_count; i++)
	{
		const size_t block_offset = bit_offset + 8 + (static_cast<size_t>(i) * 16);
		const u32 block_size = ReadLE32(&work[block_offset]);
		const u32 block_flags = ReadLE32(&work[block_offset + 4]);
		if (block_flags & 2)
			signed_blocks++;
		Console.WriteLn("[MG] PSBBN: BIT block %u size=%u flags=0x%X", static_cast<unsigned int>(i), block_size, block_flags);
	}
	if (signed_blocks == 0)
	{
		Console.Error("[MG] PSBBN: KELF has no signed BIT block");
		return false;
	}

	std::array<u8, 8> bit_signature = {};
	std::memcpy(bit_signature.data(), kbit.data(), 8);
	if (std::memcmp(kbit.data(), kbit.data() + 8, 8) != 0)
		Xor8(kbit.data() + 8, bit_signature.data());
	Xor8(kc.data(), bit_signature.data());
	if (std::memcmp(kc.data(), kc.data() + 8, 8) != 0)
		Xor8(kc.data() + 8, bit_signature.data());
	for (size_t pos = 0; pos < bit_length; pos += 8)
		Xor8(&work[bit_offset + pos], bit_signature.data());

	std::array<u8, 16> signature_master_hash_key = {};
	std::memcpy(signature_master_hash_key.data(), s_key_store.SignatureMasterKey, 8);
	std::memcpy(signature_master_hash_key.data() + 8, s_key_store.SignatureHashKey, 8);
	if (!DoubleDesEncrypt(signature_master_hash_key.data(), bit_signature.data()))
		return false;

	const size_t bit_signature_offset = bit_offset + bit_length;
	if (std::memcmp(bit_signature.data(), &work[bit_signature_offset], 8) != 0)
	{
		Console.Error("[MG] PSBBN: invalid KELF BIT signature");
		return false;
	}

	std::array<u8, 8> root_signature = header_signature;
	if (!DesEncrypt(s_key_store.RootSigMasterKey, root_signature.data()))
		return false;
	Xor8(bit_signature.data(), root_signature.data());
	if (!DesEncrypt(s_key_store.RootSigMasterKey, root_signature.data()))
		return false;
	for (u8 i = 0; i < block_count; i++)
	{
		const size_t block_offset = bit_offset + 8 + (static_cast<size_t>(i) * 16);
		if (ReadLE32(&work[block_offset + 4]) & 2)
		{
			Xor8(&work[block_offset + 8], root_signature.data());
			if (!DesEncrypt(s_key_store.RootSigMasterKey, root_signature.data()))
				return false;
		}
	}
	if (!DoubleDesDecrypt(s_key_store.RootSigHashKey, root_signature.data()))
		return false;

	const size_t root_signature_offset = bit_signature_offset + 8;
	if (std::memcmp(root_signature.data(), &work[root_signature_offset], 8) != 0)
	{
		Console.Error("[MG] PSBBN: invalid KELF root signature");
		return false;
	}

	std::memcpy(original, work.data(), data_size);
	Console.WriteLn("[MG] PSBBN: verified/decrypted MBR KELF header (size=0x%X, BIT blocks=%u)",
		declared_header_size, static_cast<unsigned int>(block_count));
	return true;
}

static void MaybeTransformBuffer()
{
	if (s_transform_in_progress)
		return;
	s_transform_in_progress = true;

	if (cdvd.mg_datatype == 1)
	{
		if (cdvd.mg_maxsize > 0 && cdvd.mg_size < cdvd.mg_maxsize)
		{
			s_header_decrypted = false;
			s_header_attempted = false;
			s_content_stage_logged = false;
		}
		else if (cdvd.mg_maxsize > 0 && cdvd.mg_size == cdvd.mg_maxsize && !s_header_attempted)
		{
			s_header_attempted = true;
			s_header_decrypted = DecryptAndVerifyMbrHeader();
		}
	}
	else if (cdvd.mg_datatype == 0 && cdvd.mg_maxsize > 0 && cdvd.mg_size == cdvd.mg_maxsize &&
		s_header_decrypted && !s_content_stage_logged)
	{
		s_content_stage_logged = true;
		Console.WriteLn("[MG] PSBBN: reached KELF content transfer; encrypted content-block handling is the next porting step");
	}

	s_transform_in_progress = false;
}
} // namespace MagicGateKelf

inline void MagicGateMaybeTransformBuffer()
{
	MagicGateKelf::MaybeTransformBuffer();
}

inline u8& MagicGateBuffer::operator[](size_t index)
{
	MagicGateMaybeTransformBuffer();
	return data[index];
}

inline const u8& MagicGateBuffer::operator[](size_t index) const
{
	MagicGateMaybeTransformBuffer();
	return data[index];
}
