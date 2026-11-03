#include "EnginePCH.h"
#include "Engine/Audio/Private/MiniaudioOpus.h"

#include <miniaudio.h>
#include <extras/decoders/libopus/miniaudio_libopus.h>

#include <array>
#include <cstddef>
#include <cstring>
#include <span>
#include <string_view>

namespace Engine {

	namespace Utils {

		// An Ogg page header (RFC 3533): 27 bytes, the segment count at byte 26, then that many lacing values.
		constexpr size_t OpusOggPageHeaderSize = 27;
		constexpr size_t OpusOggSegmentCountOffset = 26;
		constexpr size_t OpusOggMaxSegments = 255;
		// The identification header's magic (RFC 7845 section 5.1).
		constexpr std::string_view OpusHeadMagic = "OpusHead";

		// Reads exactly `bytes.size()` bytes through `onRead`; false on an error or the end of the data.
		static bool ReadExactly(ma_read_proc onRead, void* userData, std::span<std::byte> bytes)
		{
			size_t done = 0;
			while (done < bytes.size())
			{
				size_t read = 0;
				if (onRead(userData, bytes.data() + done, bytes.size() - done, &read) != MA_SUCCESS || read == 0)
					return false;
				done += read;
			}
			return true;
		}

		// Whether the data starts with an Ogg page whose first packet is an Opus identification header: AudioDecoder's
		// detection of Ogg Opus (its first Ogg packet starts with "OpusHead"), read through miniaudio's callbacks.
		static bool StartsOggOpusStream(ma_read_proc onRead, void* userData)
		{
			std::array<std::byte, OpusOggPageHeaderSize> header{};
			if (!ReadExactly(onRead, userData, header) || std::memcmp(header.data(), "OggS", 4) != 0)
				return false;
			const size_t segmentCount = std::to_integer<size_t>(header[OpusOggSegmentCountOffset]);
			std::array<std::byte, OpusOggMaxSegments> lacing{};
			if (!ReadExactly(onRead, userData, std::span(lacing).first(segmentCount)))
				return false;
			size_t packetSize = 0;
			for (size_t segment = 0; segment < segmentCount; ++segment)
			{
				packetSize += std::to_integer<size_t>(lacing[segment]);
				if (lacing[segment] != std::byte{ 0xFF })
					break;
			}
			std::array<std::byte, OpusHeadMagic.size()> magic{};
			if (packetSize < magic.size() || !ReadExactly(onRead, userData, magic))
				return false;
			return std::memcmp(magic.data(), OpusHeadMagic.data(), magic.size()) == 0;
		}

	}

	MiniaudioOpusBackend::MiniaudioOpusBackend()
	{
		m_VTable.onInit = &MiniaudioOpusBackend::OnInit;
		m_VTable.onUninit = &MiniaudioOpusBackend::OnUninit;
		m_VTables = { &m_VTable };
	}

	ma_result MiniaudioOpusBackend::OnInit(void* /*userData*/, ma_read_proc onRead, ma_seek_proc onSeek, ma_tell_proc onTell, void* readSeekTellUserData,
		const ma_decoding_backend_config* config, const ma_allocation_callbacks* allocationCallbacks, ma_data_source** backend)
	{
		if (onRead == nullptr || onSeek == nullptr || backend == nullptr)
			return MA_INVALID_ARGS;
		const bool opus = Utils::StartsOggOpusStream(onRead, readSeekTellUserData);
		if (const ma_result rewound = onSeek(readSeekTellUserData, 0, ma_seek_origin_start); rewound != MA_SUCCESS)
			return rewound;
		if (!opus)
			return MA_INVALID_FILE;

		// The decoder converts the backend's s16 to its own output format (f32 for the engine).
		ma_decoding_backend_config sixteenBit = config != nullptr ? *config : ma_decoding_backend_config_init(ma_format_s16, 0);
		sixteenBit.preferredFormat = ma_format_s16;
		return ma_decoding_backend_libopus->onInit(nullptr, onRead, onSeek, onTell, readSeekTellUserData, &sixteenBit, allocationCallbacks, backend);
	}

	void MiniaudioOpusBackend::OnUninit(void* /*userData*/, ma_data_source* backend, const ma_allocation_callbacks* allocationCallbacks)
	{
		ma_decoding_backend_libopus->onUninit(nullptr, backend, allocationCallbacks);
	}

}
