// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "SIO/Memcard/MemoryCardProtocol.h"

#include "SIO/Sio.h"
#include "SIO/Sio2.h"
#include "SIO/Sio0.h"
#include "Crypto/MagicGateCrypto.h"

#include "common/Assertions.h"
#include "common/Console.h"
#include "common/Path.h"
#include "ps2/BiosTools.h"

#include <algorithm>
#include <cstring>

#define MC_LOG_ENABLE 0
#define MC_LOG if (MC_LOG_ENABLE) DevCon

#define PS1_FAIL() if (this->PS1Fail()) return;

MemoryCardProtocol g_MemoryCardProtocol;

void MemoryCardProtocol::ResetAuthState()
{
	m_auth_crypt_state = AuthCryptState::Idle;
	m_auth_last_command = MemcardCommand::NOT_SET;
	authCryptBuffer.fill(0);
}

void MemoryCardProtocol::BeginCommand(u8 commandByte, bool cardPresent)
{
	if (!cardPresent)
	{
		ResetAuthForDisconnect();
		return;
	}

	if (commandByte == MemcardCommand::AUTH_F3)
	{
		ResetAuthState();
		return;
	}

	if (commandByte != m_auth_last_command)
		m_auth_crypt_state = AuthCryptState::Idle;

	m_auth_last_command = commandByte;
}

// Check if the memcard is for PS1, and if we are working on a command sent over SIO2.
// If so, return dead air.
bool MemoryCardProtocol::PS1Fail()
{
	if (mcd->IsPSX() && g_Sio2.commandLength > 0)
	{
		while (g_Sio2FifoOut.size() < g_Sio2.commandLength)
		{
			g_Sio2FifoOut.push_back(0x00);
		}

		return true;
	}

	return false;
}

// A repeated pattern in memcard commands is to pad with zero bytes,
// then end with 0x2b and terminator bytes. This function is a shortcut for that.
void MemoryCardProtocol::The2bTerminator(size_t length)
{
	while (g_Sio2FifoOut.size() < length - 2)
	{
		g_Sio2FifoOut.push_back(0x00);
	}

	g_Sio2FifoOut.push_back(0x2b);
	g_Sio2FifoOut.push_back(mcd->term);
}

// After one read or write, the memcard is almost certainly going to be issued a new read or write
// for the next segment of the same sector. Bump the transferAddr to where that segment begins.
// If it is the end and a new sector is being accessed, the SetSector function will deal with
// both sectorAddr and transferAddr.
void MemoryCardProtocol::ReadWriteIncrement(size_t length)
{
	mcd->transferAddr += length;
}

void MemoryCardProtocol::RecalculatePS1Addr()
{
	mcd->sectorAddr = ((ps1McState.sectorAddrMSB << 8) | ps1McState.sectorAddrLSB);
	mcd->goodSector = (mcd->sectorAddr <= 0x03ff);
	mcd->transferAddr = 128 * mcd->sectorAddr;
}

void MemoryCardProtocol::ResetAuthForDisconnect()
{
	ResetAuthState();
	authMaterialLoaded = false;
	m_current_keyset = MagicGateKeyset::Retail;
}

void MemoryCardProtocol::NotifyCommandStart(u8 commandByte, bool cardPresent)
{
	BeginCommand(commandByte, cardPresent);
}

void MemoryCardProtocol::ResetPS1State()
{
	ps1McState.currentByte = 2;
	ps1McState.sectorAddrMSB = 0;
	ps1McState.sectorAddrLSB = 0;
	ps1McState.checksum = 0;
	ps1McState.expectedChecksum = 0;
	memset(ps1McState.buf.data(), 0, ps1McState.buf.size());
}

void MemoryCardProtocol::Probe()
{
	MC_LOG.WriteLn("%s", __FUNCTION__);
	PS1_FAIL();

	if (!mcd->IsPresent())
	{
		g_Sio2FifoOut.push_back(0xff);
		g_Sio2FifoOut.push_back(0xff);
		g_Sio2FifoOut.push_back(0xff);
		g_Sio2FifoOut.push_back(0xff);
	}
	else
	{
		The2bTerminator(4);
	}
}

void MemoryCardProtocol::UnknownWriteDeleteEnd()
{
	MC_LOG.WriteLn("%s", __FUNCTION__);
	PS1_FAIL();
	The2bTerminator(4);
}

void MemoryCardProtocol::SetSector()
{
	MC_LOG.WriteLn("%s", __FUNCTION__);
	PS1_FAIL();
	const u8 sectorLSB = g_Sio2FifoIn.front();
	g_Sio2FifoIn.pop_front();
	const u8 sector2nd = g_Sio2FifoIn.front();
	g_Sio2FifoIn.pop_front();
	const u8 sector3rd = g_Sio2FifoIn.front();
	g_Sio2FifoIn.pop_front();
	const u8 sectorMSB = g_Sio2FifoIn.front();
	g_Sio2FifoIn.pop_front();
	const u8 expectedChecksum = g_Sio2FifoIn.front();
	g_Sio2FifoIn.pop_front();

	u8 computedChecksum = sectorLSB ^ sector2nd ^ sector3rd ^ sectorMSB;
	mcd->goodSector = (computedChecksum == expectedChecksum);

	if (!mcd->goodSector)
	{
		Console.Warning("%s() Warning! Memcard sector checksum failed! (Expected %02X != Actual %02X) Please report to the PCSX2 team!", __FUNCTION__, expectedChecksum, computedChecksum);
	}

	u32 newSector = sectorLSB | (sector2nd << 8) | (sector3rd << 16) | (sectorMSB << 24);
	mcd->sectorAddr = newSector;

	McdSizeInfo info;
	mcd->GetSizeInfo(info);
	mcd->transferAddr = (info.SectorSize + 16) * mcd->sectorAddr;

	The2bTerminator(9);
}

void MemoryCardProtocol::GetSpecs()
{
	MC_LOG.WriteLn("%s", __FUNCTION__);
	PS1_FAIL();
	//u8 checksum = 0x00;
	McdSizeInfo info;
	mcd->GetSizeInfo(info);
	g_Sio2FifoOut.push_back(0x2b);
	
	const u8 sectorSizeLSB = (info.SectorSize & 0xff);
	//checksum ^= sectorSizeLSB;
	g_Sio2FifoOut.push_back(sectorSizeLSB);

	const u8 sectorSizeMSB = (info.SectorSize >> 8);
	//checksum ^= sectorSizeMSB;
	g_Sio2FifoOut.push_back(sectorSizeMSB);

	const u8 eraseBlockSizeLSB = (info.EraseBlockSizeInSectors & 0xff);
	//checksum ^= eraseBlockSizeLSB;
	g_Sio2FifoOut.push_back(eraseBlockSizeLSB);

	const u8 eraseBlockSizeMSB = (info.EraseBlockSizeInSectors >> 8);
	//checksum ^= eraseBlockSizeMSB;
	g_Sio2FifoOut.push_back(eraseBlockSizeMSB);

	const u8 sectorCountLSB = (info.McdSizeInSectors & 0xff);
	//checksum ^= sectorCountLSB;
	g_Sio2FifoOut.push_back(sectorCountLSB);

	const u8 sectorCount2nd = (info.McdSizeInSectors >> 8);
	//checksum ^= sectorCount2nd;
	g_Sio2FifoOut.push_back(sectorCount2nd);

	const u8 sectorCount3rd = (info.McdSizeInSectors >> 16);
	//checksum ^= sectorCount3rd;
	g_Sio2FifoOut.push_back(sectorCount3rd);

	const u8 sectorCountMSB = (info.McdSizeInSectors >> 24);
	//checksum ^= sectorCountMSB;
	g_Sio2FifoOut.push_back(sectorCountMSB);
	
	g_Sio2FifoOut.push_back(info.Xor);
	g_Sio2FifoOut.push_back(mcd->term);
}

void MemoryCardProtocol::SetTerminator()
{
	MC_LOG.WriteLn("%s", __FUNCTION__);
	PS1_FAIL();
	mcd->term = g_Sio2FifoIn.front();
	g_Sio2FifoIn.pop_front();
	g_Sio2FifoOut.push_back(0x00);
	g_Sio2FifoOut.push_back(0x2b);
	g_Sio2FifoOut.push_back(mcd->term);
}

// This one is a bit unusual. Old and new versions of MCMAN seem to handle this differently.
// Some commands may check [4] for the terminator. Others may check [3]. Typically, older
// MCMAN revisions will exclusively check [4], and newer revisions will check both [3] and [4]
// for different values. In all cases, they expect to see a valid terminator value.
//
// Also worth noting old revisions of MCMAN will not set anything other than 0x55 for the terminator,
// while newer revisions will set the terminator to another value (most commonly 0x5a).
void MemoryCardProtocol::GetTerminator()
{
	MC_LOG.WriteLn("%s", __FUNCTION__);
	PS1_FAIL();
	g_Sio2FifoOut.push_back(0x2b);
	g_Sio2FifoOut.push_back(mcd->term);
	g_Sio2FifoOut.push_back(mcd->term);
}

void MemoryCardProtocol::WriteData()
{
	MC_LOG.WriteLn("%s", __FUNCTION__);
	PS1_FAIL();
	g_Sio2FifoOut.push_back(0x00);
	g_Sio2FifoOut.push_back(0x2b);
	const u8 writeLength = g_Sio2FifoIn.front();
	g_Sio2FifoIn.pop_front();
	u8 checksum = 0x00;
	std::vector<u8> buf;

	for (size_t writeCounter = 0; writeCounter < writeLength; writeCounter++)
	{
		const u8 writeByte = g_Sio2FifoIn.front();
		g_Sio2FifoIn.pop_front();
		checksum ^= writeByte;
		buf.push_back(writeByte);
		g_Sio2FifoOut.push_back(0x00);
	}

	mcd->Write(buf.data(), buf.size());
	g_Sio2FifoOut.push_back(checksum);
	g_Sio2FifoOut.push_back(mcd->term);

	ReadWriteIncrement(writeLength);

	MemcardBusy::SetBusy();
}

void MemoryCardProtocol::ReadData()
{
	MC_LOG.WriteLn("%s", __FUNCTION__);
	PS1_FAIL();
	const u8 readLength = g_Sio2FifoIn.front();
	g_Sio2FifoIn.pop_front();
	g_Sio2FifoOut.push_back(0x00);
	g_Sio2FifoOut.push_back(0x2b);
	std::vector<u8> buf;
	buf.resize(readLength);
	mcd->Read(buf.data(), buf.size());
	u8 checksum = 0x00;

	for (const u8 readByte : buf)
	{
		checksum ^= readByte;
		g_Sio2FifoOut.push_back(readByte);
	}

	g_Sio2FifoOut.push_back(checksum);
	g_Sio2FifoOut.push_back(mcd->term);

	ReadWriteIncrement(readLength);
}

u8 MemoryCardProtocol::PS1Read(u8 data)
{
	MC_LOG.WriteLn("%s", __FUNCTION__);

	if (!mcd->IsPresent())
	{
		return 0xff;
	}

	bool sendAck = true;
	u8 ret = 0;

	switch (ps1McState.currentByte)
	{
		case 2:
			ret = 0x5a;
			break;
		case 3:
			ret = 0x5d;
			break;
		case 4:
			ps1McState.sectorAddrMSB = data;
			ret = 0x00;
			break;
		case 5:
			ps1McState.sectorAddrLSB = data;
			ret = 0x00;
			RecalculatePS1Addr();
			break;
		case 6:
			ret = 0x5c;
			break;
		case 7:
			ret = 0x5d;
			break;
		case 8:
			ret = ps1McState.sectorAddrMSB;
			break;
		case 9:
			ret = ps1McState.sectorAddrLSB;
			break;
		case 138:
			ret = ps1McState.checksum;
			break;
		case 139:
			ret = 0x47;
			sendAck = false;
			break;
		case 10:
			ps1McState.checksum = ps1McState.sectorAddrMSB ^ ps1McState.sectorAddrLSB;
			mcd->Read(ps1McState.buf.data(), ps1McState.buf.size());
			[[fallthrough]];
		default:
			ret = ps1McState.buf[ps1McState.currentByte - 10];
			ps1McState.checksum ^= ret;
			break;
	}

	g_Sio0.SetAcknowledge(sendAck);

	ps1McState.currentByte++;
	return ret;
}

u8 MemoryCardProtocol::PS1State(u8 data)
{
	Console.Error("%s(%02X) I do not exist, please change that ASAP.", __FUNCTION__, data);
	pxFail("Missing PS1State handler");
	return 0x00;
}

u8 MemoryCardProtocol::PS1Write(u8 data)
{
	MC_LOG.WriteLn("%s", __FUNCTION__);
	bool sendAck = true;
	u8 ret = 0;

	switch (ps1McState.currentByte)
	{
		case 2:
			ret = 0x5a;
			break;
		case 3:
			ret = 0x5d;
			break;
		case 4:
			ps1McState.sectorAddrMSB = data;
			ret = 0x00;
			break;
		case 5:
			ps1McState.sectorAddrLSB = data;
			ret = 0x00;
			RecalculatePS1Addr();
			break;
		case 134:
			ps1McState.expectedChecksum = data;
			ret = 0;
			break;
		case 135:
			ret = 0x5c;
			break;
		case 136:
			ret = 0x5d;
			break;
		case 137:
			if (!mcd->goodSector)
			{
				ret = 0xff;
			}
			else if (ps1McState.expectedChecksum != ps1McState.checksum)
			{
				ret = 0x4e;
			}
			else
			{
				mcd->Write(ps1McState.buf.data(), ps1McState.buf.size());
				ret = 0x47;
				// Clear the "directory unread" bit of the flag byte. Per no$psx, this is cleared
				// on writes, not reads.
				mcd->FLAG &= 0x07;
			}

			sendAck = false;
			break;
		case 6:
			ps1McState.checksum = ps1McState.sectorAddrMSB ^ ps1McState.sectorAddrLSB;
			[[fallthrough]];
		default:
			ps1McState.buf[ps1McState.currentByte - 6] = data;
			ps1McState.checksum ^= data;
			ret = 0x00;
			break;
	}

	g_Sio0.SetAcknowledge(sendAck);
	ps1McState.currentByte++;

	MemcardBusy::SetBusy();
	return ret;
}

u8 MemoryCardProtocol::PS1Pocketstation(u8 data)
{
	MC_LOG.WriteLn("%s", __FUNCTION__);
	g_Sio0.SetAcknowledge(false);
	return 0x00;
}

void MemoryCardProtocol::ReadWriteEnd()
{
	MC_LOG.WriteLn("%s", __FUNCTION__);
	PS1_FAIL();
	The2bTerminator(4);
}

void MemoryCardProtocol::EraseBlock()
{
	MC_LOG.WriteLn("%s", __FUNCTION__);
	PS1_FAIL();
	mcd->EraseBlock();
	The2bTerminator(4);

	MemcardBusy::SetBusy();
}

void MemoryCardProtocol::UnknownBoot()
{
	MC_LOG.WriteLn("%s", __FUNCTION__);
	PS1_FAIL();
	The2bTerminator(5);
}

void MemoryCardProtocol::AuthXorDataFrame()
{
	u8 xorResult = 0x00;
	g_Sio2FifoOut.push_back(0x00);
	g_Sio2FifoOut.push_back(0x2b);

	for (size_t xorCounter = 0; xorCounter < 8; xorCounter++)
	{
		const u8 toXOR = g_Sio2FifoIn.empty() ? 0x00 : g_Sio2FifoIn.front();
		if (!g_Sio2FifoIn.empty())
			g_Sio2FifoIn.pop_front();
		xorResult ^= toXOR;
		g_Sio2FifoOut.push_back(0x00);
	}

	g_Sio2FifoOut.push_back(xorResult);
	g_Sio2FifoOut.push_back(mcd->term);
}

void MemoryCardProtocol::AuthXor()
{
	MC_LOG.WriteLn("%s", __FUNCTION__);
	PS1_FAIL();
	const u8 modeByte = g_Sio2FifoIn.empty() ? 0x00 : g_Sio2FifoIn.front();
	if (!g_Sio2FifoIn.empty())
		g_Sio2FifoIn.pop_front();
	switch (modeByte)
	{
		// Behavior derived from PCSX2 PR #4274: XOR phase with 0x2B+terminator framing.
		case MemcardAuthMode::XOR_DATA_FRAME_1:
		case MemcardAuthMode::XOR_DATA_FRAME_2:
		case MemcardAuthMode::XOR_DATA_FRAME_4:
		case MemcardAuthMode::XOR_DATA_FRAME_0F:
		case MemcardAuthMode::XOR_DATA_FRAME_11:
		case MemcardAuthMode::XOR_DATA_FRAME_13:
			AuthXorDataFrame();
			break;
		case 0x00:
		case 0x03:
		case 0x05:
		case 0x08:
		case 0x09:
		case 0x0a:
		case 0x0c:
		case 0x0d:
		case 0x0e:
		case 0x10:
		case 0x12:
		case 0x14:
			The2bTerminator(5);
			break;
		case 0x06:
		case 0x07:
		case 0x0b:
			The2bTerminator(14);
			break;
		default:
			Console.Warning("%s(queue) Unexpected modeByte (%02X), please report to the PCSX2 team", __FUNCTION__, modeByte);
			break;
	}
}

void MemoryCardProtocol::AuthCrypt()
{
	MC_LOG.WriteLn("%s", __FUNCTION__);
	PS1_FAIL();
	const u8 modeByte = g_Sio2FifoIn.empty() ? 0x00 : g_Sio2FifoIn.front();
	if (!g_Sio2FifoIn.empty())
		g_Sio2FifoIn.pop_front();

	switch (modeByte)
	{
		// Behavior derived from PCSX2 PR #4274: command windows for crypt receive/send.
		case MemcardAuthMode::CRYPT_REQUEST_40:
		case MemcardAuthMode::CRYPT_REQUEST_42:
		case MemcardAuthMode::CRYPT_REQUEST_50:
		case MemcardAuthMode::CRYPT_REQUEST_52:
			m_auth_crypt_state = AuthCryptState::AwaitingPayload;
			AuthXorDataFrame();
			break;
		case MemcardAuthMode::CRYPT_PAYLOAD_DECRYPT:
		case MemcardAuthMode::CRYPT_PAYLOAD_ENCRYPT:
		{
			DEV_LOG("MagicGate[SIO]: crypt payload received (mode=0x{:02X}, keyset='{}', material_loaded={})",
				modeByte, m_auth_provider.GetKeysetName(m_current_keyset), authMaterialLoaded);
			std::array<u8, 9> incoming = {};
			u8 incoming_size = 0;
			while (!g_Sio2FifoIn.empty() && incoming_size < incoming.size())
			{
				incoming[incoming_size++] = g_Sio2FifoIn.front();
				g_Sio2FifoIn.pop_front();
			}

			if (incoming_size >= 8)
			{
				std::copy_n(incoming.begin(), 8, authCryptBuffer.begin());
				m_auth_crypt_state = AuthCryptState::PayloadReady;
			}
			else
			{
				authCryptBuffer.fill(0);
				m_auth_crypt_state = AuthCryptState::Idle;
			}

			// Adapted crypto backend for PR #4274 behavior: shared internal DES/2DES API.
			if (authMaterialLoaded && m_auth_crypt_state == AuthCryptState::PayloadReady)
			{
				DEV_LOG("MagicGate[SIO]: pre-decrypt handshake milestone reached (mode=0x{:02X}).", modeByte);
				const MagicGateMaterial& material = m_auth_provider.GetMaterial(m_current_keyset);
				MagicGateCrypto::Block8 input = {};
				MagicGateCrypto::Block8 output = {};
				std::copy_n(authCryptBuffer.begin(), 8, input.begin());

				if (modeByte == MemcardAuthMode::CRYPT_PAYLOAD_DECRYPT)
					MagicGateCrypto::TwoDesDecrypt(material.key, input, &output);
				else
					MagicGateCrypto::TwoDesEncrypt(material.key, input, &output);

				std::copy_n(output.begin(), 8, authCryptBuffer.begin());
			}

			The2bTerminator(5);
			break;
		}
		case MemcardAuthMode::CRYPT_RESPONSE_43:
		case MemcardAuthMode::CRYPT_RESPONSE_53:
			if (!authMaterialLoaded)
				WARNING_LOG("MagicGate: crypt response requested without available key material; returning framed zero data.");
			g_Sio2FifoOut.push_back(0x00);
			g_Sio2FifoOut.push_back(0x2b);
			for (const u8 data : authCryptBuffer)
				g_Sio2FifoOut.push_back(data);
			g_Sio2FifoOut.push_back(mcd->term);
			break;
		default:
			m_auth_crypt_state = AuthCryptState::Idle;
			The2bTerminator(5);
			break;
	}
}

void MemoryCardProtocol::AuthF3()
{
	MC_LOG.WriteLn("%s", __FUNCTION__);
	PS1_FAIL();

	if (!mcd->IsPresent())
	{
		g_Sio2FifoOut.push_back(0xff);
		g_Sio2FifoOut.push_back(0xff);
		g_Sio2FifoOut.push_back(0xff);
		g_Sio2FifoOut.push_back(0xff);
	}
	else
	{
		ResetAuthState();
		m_auth_provider.Refresh();
		m_current_keyset = m_auth_provider.GetDefaultKeyset();
		const MagicGateMaterial& material = m_auth_provider.GetMaterial(m_current_keyset);
		authMaterialLoaded = material.valid;
		authCryptBuffer = material.iv;

		// PCSX2 PR #4274 reviewer note by balika011 identified Namco System 246/256
		// failures before decrypt; this card-init trace is the first milestone to
		// quickly distinguish SIO auth setup from later CDVD decrypt stages.
		DEV_LOG("MagicGate[SIO]: card init milestone (AUTH_F3) keyset='{}' material_loaded={} bios='{}'",
			m_auth_provider.GetKeysetName(m_current_keyset), authMaterialLoaded, Path::GetFileName(BiosPath));

		if (!authMaterialLoaded)
		{
			WARNING_LOG("MagicGate: keyset '{}' unavailable for BIOS '{}'. Keeping auth framing but crypt payload will be inert.",
				m_auth_provider.GetKeysetName(m_current_keyset), Path::GetFileName(BiosPath));
		}

		mcd->term = Terminator::READY;

		// This provider-driven path is the modernized replacement for PR #4274's
		// eks.bin/cks.bin/kek.bin/civ.bin lookup strategy.
		The2bTerminator(5);
	}
}

void MemoryCardProtocol::AuthKeySelect()
{
	MC_LOG.WriteLn("%s", __FUNCTION__);
	PS1_FAIL();
	const u8 keyIndex = g_Sio2FifoIn.empty() ? 0x00 : g_Sio2FifoIn.front();
	if (!g_Sio2FifoIn.empty())
		g_Sio2FifoIn.pop_front();
	MagicGateKeyset requested_keyset = m_current_keyset;

	if (keyIndex == 0x01)
		requested_keyset = MagicGateKeyset::Retail;
	else if (keyIndex == 0x02)
		requested_keyset = MagicGateKeyset::Dev;
	else if (keyIndex == 0x03)
		requested_keyset = MagicGateKeyset::Proto;
	else if (keyIndex == 0x04)
		requested_keyset = MagicGateKeyset::Arcade;

	const MagicGateMaterial& material = m_auth_provider.GetMaterial(requested_keyset);
	if (!material.valid)
	{
		WARNING_LOG("MagicGate: requested keyset '{}' is unavailable. Continuing with inert auth payloads.",
			m_auth_provider.GetKeysetName(requested_keyset));
		authMaterialLoaded = false;
		authCryptBuffer.fill(0);
	}
	else
	{
		m_current_keyset = requested_keyset;
		authMaterialLoaded = true;
		authCryptBuffer = material.iv;
	}

	// legacy equivalent of PR #4274 SIO_MEMCARD_CRYPT / SIO_MEMCARD_KEY_SELECT.
	// Behavior derived from PCSX2 PR #4274: key select command acks with short 0x2B frame.
	The2bTerminator(5);
}

void MemoryCardProtocol::AuthF7()
{
	AuthKeySelect();
}
