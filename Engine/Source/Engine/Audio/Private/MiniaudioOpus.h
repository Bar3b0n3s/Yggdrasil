#pragma once

#include "Engine/Core/Base.h"

#include <miniaudio.h>

#include <array>

// The Ogg Opus decoding backend the Audio module registers with miniaudio's decoders (Architecture §10.1;
// Docs/Decisions/0034-m23-contract.md decision 16): miniaudio's unmodified libopus backend
// (Vendor/miniaudio/extras/decoders/libopus, over opusfile and the fixed-point libopus), registered through a vtable of
// its own that
//   - claims only data whose first Ogg page starts with an Opus identification header ("OggS", then an "OpusHead"
//     packet), as AudioDecoder detects Ogg Opus, so an ma_decoder of unknown encoding, which tries the custom backends
//     first (the resource manager's streams), never hands opusfile a WAV, FLAC, MP3 or Ogg Vorbis file; and
//   - asks the libopus backend for ma_format_s16: opusfile's op_read, the 16-bit samples of the fixed-point decoder,
//     which miniaudio's decoder converts to f32 exactly (x / 32768), so every configuration and CPU decodes the same
//     samples.

namespace Engine {

	// One vtable for ma_decoder_config::ppCustomBackendVTables or ma_resource_manager_config::ppCustomDecodingBackendVTables.
	// Not copyable or movable: a decoder initialized through it keeps the vtable's address, so the object outlives every
	// decoder (and resource manager) that uses it. Thread-safe: the vtable is never written after construction.
	class MiniaudioOpusBackend
	{
	public:
		MiniaudioOpusBackend();

		MiniaudioOpusBackend(const MiniaudioOpusBackend&) = delete;
		MiniaudioOpusBackend& operator=(const MiniaudioOpusBackend&) = delete;

		// The vtable list and its length for a decoder or resource manager config.
		[[nodiscard]] ma_decoding_backend_vtable** GetVTables() { return m_VTables.data(); }
		[[nodiscard]] ma_uint32 GetVTableCount() const { return static_cast<ma_uint32>(m_VTables.size()); }

		// Whether `decoder` decodes through this backend, rather than through a built-in decoder miniaudio tried after the
		// custom backends refused the data.
		[[nodiscard]] bool Decodes(const ma_decoder& decoder) const { return decoder.pBackendVTable == &m_VTable; }
	private:
		static ma_result OnInit(void* userData, ma_read_proc onRead, ma_seek_proc onSeek, ma_tell_proc onTell, void* readSeekTellUserData,
			const ma_decoding_backend_config* config, const ma_allocation_callbacks* allocationCallbacks, ma_data_source** backend);
		static void OnUninit(void* userData, ma_data_source* backend, const ma_allocation_callbacks* allocationCallbacks);
	private:
		ma_decoding_backend_vtable m_VTable{};
		std::array<ma_decoding_backend_vtable*, 1> m_VTables{};
	};

}
