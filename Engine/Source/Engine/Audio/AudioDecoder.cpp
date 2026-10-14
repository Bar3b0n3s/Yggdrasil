#include "EnginePCH.h"
#include "Engine/Audio/AudioDecoder.h"

#include "Engine/Audio/Private/MiniaudioSupport.h"
#include "Engine/Core/Assert.h"

#include <miniaudio.h>

#include <algorithm>
#include <array>
#include <bit>
#include <climits>
#include <cmath>
#include <cstring>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Engine {

	namespace {

		// The formats a decode accepts, for the messages of bytes that are none of them.
		constexpr std::string_view SupportedFormats = "WAV, FLAC, MP3 or Ogg Vorbis";
		constexpr uint32_t MinSampleRate = 8000;
		constexpr uint32_t MaxSampleRate = 192000;
		// Frames decoded per ma_decoder_read_pcm_frames call.
		constexpr uint64_t DecodeChunkFrames = 4096;
		// Ogg pages (RFC 3533): a 27-byte header with the flags at byte 5 and the segment count at byte 26, then the lacing
		// values and the body.
		constexpr size_t OggPageHeaderSize = 27;
		constexpr uint8_t OggContinuedPacket = 0x01;
		constexpr uint8_t OggFirstPage = 0x02;
		constexpr uint8_t OggEndOfStream = 0x04;
		// The Vorbis header packets (Vorbis I specification, section 4.2): a type byte and "vorbis", the identification
		// header alone on the first page in 30 bytes, then the comment and setup headers.
		constexpr size_t VorbisHeaderPrefixSize = 7;
		constexpr size_t VorbisIdentificationSize = 30;
		constexpr uint8_t VorbisCommentPacket = 3;
		constexpr uint8_t VorbisSetupPacket = 5;
		// stb_vorbis sizes its codebook tables (codewords, lookup multiplicands) and each residue's class data from a
		// codebook's entries times dimensions, in int. A codebook of more values than this, far beyond any encoder's, is
		// refused, so those tables stay within a few MiB and their sizes never overflow.
		constexpr uint64_t MaxVorbisCodebookValues = uint64_t{ 1 } << 20;

		// One page of an Ogg file: its header flags, its lacing values and its body.
		struct OggPage
		{
			uint8_t Flags = 0;
			std::span<const std::byte> Lacing{};
			std::span<const std::byte> Body{};
		};

		// A segment of an Ogg file: a page, a segment of it and where that segment starts in the page's body.
		struct OggPosition
		{
			size_t Page = 0;
			size_t Segment = 0;
			size_t BodyOffset = 0;
		};

		// stb_vorbis's setup-header bit reader (get_bits): bits packed from the least significant bit of each byte, reads of
		// more than 24 bits split in two; once a read runs past the end of the packet, it and every later read return 0.
		class VorbisBitReader
		{
		public:
			explicit VorbisBitReader(std::span<const std::byte> bytes)
				: m_Bytes(bytes)
			{
			}

			[[nodiscard]] bool IsExhausted() const { return m_Exhausted; }

			// The next `count` (0 to 32) bits; between reads at most 7 bits are buffered, so a read of more than 24 always splits.
			[[nodiscard]] uint32_t Read(uint32_t count)
			{
				if (m_Exhausted)
					return 0;
				if (count > 24)
				{
					const uint32_t low = Read(24);
					return low + (Read(count - 24) << 24);
				}
				if (m_ValidBits < count)
				{
					if (m_ValidBits == 0)
						m_Accumulator = 0;
					while (m_ValidBits < count)
					{
						if (m_Position >= m_Bytes.size())
						{
							m_Exhausted = true;
							return 0;
						}
						m_Accumulator += std::to_integer<uint32_t>(m_Bytes[m_Position++]) << m_ValidBits;
						m_ValidBits += 8;
					}
				}
				const uint32_t value = m_Accumulator & ((uint32_t{ 1 } << count) - 1);
				m_Accumulator >>= count;
				m_ValidBits -= count;
				return value;
			}
		private:
			std::span<const std::byte> m_Bytes{};
			size_t m_Position = 0;
			uint32_t m_Accumulator = 0;
			uint32_t m_ValidBits = 0;
			bool m_Exhausted = false;
		};

		// An initialized ma_decoder, uninitialized on destruction. Not copyable or movable (miniaudio keeps its address).
		class ScopedDecoder
		{
		public:
			ScopedDecoder() = default;
			~ScopedDecoder()
			{
				if (m_Initialized)
					ma_decoder_uninit(&m_Decoder);
			}

			ScopedDecoder(const ScopedDecoder&) = delete;
			ScopedDecoder& operator=(const ScopedDecoder&) = delete;

			[[nodiscard]] ma_result Initialize(std::span<const std::byte> bytes, ma_encoding_format format)
			{
				ma_decoder_config config = ma_decoder_config_init(ma_format_f32, 0, 0);
				config.encodingFormat = format;
				const ma_result result = ma_decoder_init_memory(bytes.data(), bytes.size(), &config, &m_Decoder);
				m_Initialized = result == MA_SUCCESS;
				return result;
			}

			[[nodiscard]] ma_decoder& Get() { return m_Decoder; }
		private:
			ma_decoder m_Decoder{};
			bool m_Initialized = false;
		};

	}

	namespace Utils {

		static bool HasPrefix(std::span<const std::byte> bytes, size_t offset, std::string_view prefix)
		{
			if (bytes.size() < offset || bytes.size() - offset < prefix.size())
				return false;
			return std::memcmp(bytes.data() + offset, prefix.data(), prefix.size()) == 0;
		}

		static uint8_t ByteAt(std::span<const std::byte> bytes, size_t offset)
		{
			return static_cast<uint8_t>(bytes[offset]);
		}

		// An MPEG audio frame header at `offset`: the 11-bit sync, a defined version and layer, and a bitrate and sample rate
		// index that are not reserved.
		static bool IsMpegFrameHeader(std::span<const std::byte> bytes, size_t offset)
		{
			if (bytes.size() < offset || bytes.size() - offset < 4)
				return false;
			const uint8_t first = ByteAt(bytes, offset);
			const uint8_t second = ByteAt(bytes, offset + 1);
			const uint8_t third = ByteAt(bytes, offset + 2);
			const bool sync = first == 0xFF && (second & 0xE0) == 0xE0;
			const bool version = ((second >> 3) & 0x03) != 0x01;
			const bool layer = ((second >> 1) & 0x03) != 0x00;
			const bool bitrate = (third >> 4) != 0x0F;
			const bool sampleRate = ((third >> 2) & 0x03) != 0x03;
			return sync && version && layer && bitrate && sampleRate;
		}

		// The size of an ID3v2 tag at the start of `bytes` (header, synchsafe size and optional footer); 0 without one.
		static size_t GetId3TagSize(std::span<const std::byte> bytes)
		{
			if (!HasPrefix(bytes, 0, "ID3") || bytes.size() < 10)
				return 0;
			size_t size = 0;
			for (size_t index = 6; index < 10; ++index)
				size = (size << 7) | (ByteAt(bytes, index) & 0x7F);
			const bool footer = (ByteAt(bytes, 5) & 0x10) != 0;
			return 10 + size + (footer ? 10 : 0);
		}

		// The first packet of an Ogg stream's first page (the codec's identification header), or an empty span when the page
		// is cut short.
		static std::span<const std::byte> GetFirstOggPacket(std::span<const std::byte> bytes)
		{
			constexpr size_t PageHeaderSize = 27;
			if (bytes.size() < PageHeaderSize)
				return {};
			const size_t segmentCount = ByteAt(bytes, 26);
			if (bytes.size() < PageHeaderSize + segmentCount)
				return {};
			size_t packetSize = 0;
			for (size_t segment = 0; segment < segmentCount; ++segment)
			{
				const uint8_t lacing = ByteAt(bytes, PageHeaderSize + segment);
				packetSize += lacing;
				if (lacing < 255)
					break;
			}
			const size_t start = PageHeaderSize + segmentCount;
			return bytes.subspan(start, std::min(packetSize, bytes.size() - start));
		}

		// The codec of an Ogg stream that is not Vorbis, from its identification header, for the refusal's message.
		static std::string_view DescribeOggCodec(std::span<const std::byte> packet)
		{
			if (HasPrefix(packet, 0, "OpusHead"))
				return "Opus";
			if (HasPrefix(packet, 0, "\177FLAC")) // 0x7F, then "FLAC"
				return "FLAC";
			if (HasPrefix(packet, 0, "Speex   "))
				return "Speex";
			return "an unknown codec";
		}

		// The container of `bytes`, from its magic numbers (never from a file extension). Errors: Parse naming the supported
		// formats, or the Ogg codec that is not Vorbis with a conversion hint.
		static Result<EncodedAudioFormat> DetectFormat(std::span<const std::byte> bytes)
		{
			if ((HasPrefix(bytes, 0, "RIFF") || HasPrefix(bytes, 0, "RIFX") || HasPrefix(bytes, 0, "RF64")) && HasPrefix(bytes, 8, "WAVE"))
				return EncodedAudioFormat::Wav;
			if (HasPrefix(bytes, 0, "fLaC"))
				return EncodedAudioFormat::Flac;
			if (HasPrefix(bytes, 0, "OggS"))
			{
				const std::span<const std::byte> packet = GetFirstOggPacket(bytes);
				if (HasPrefix(packet, 0, "\x01vorbis"))
					return EncodedAudioFormat::Vorbis;
				return std::unexpected(Error(ErrorCode::Parse, std::format("the Ogg file holds {}, not Vorbis", DescribeOggCodec(packet)))
						.WithHint(std::format("convert the file to {}", SupportedFormats)));
			}
			if (const size_t tag = GetId3TagSize(bytes); tag != 0)
				return HasPrefix(bytes, tag, "fLaC") ? EncodedAudioFormat::Flac : EncodedAudioFormat::Mp3;
			if (IsMpegFrameHeader(bytes, 0))
				return EncodedAudioFormat::Mp3;
			return std::unexpected(
				Error(ErrorCode::Parse, std::format("the data is not a supported audio file: expected a {} file", SupportedFormats)));
		}

		static ma_encoding_format ToMiniaudioFormat(EncodedAudioFormat format)
		{
			switch (format)
			{
				case EncodedAudioFormat::Wav:    return ma_encoding_format_wav;
				case EncodedAudioFormat::Flac:   return ma_encoding_format_flac;
				case EncodedAudioFormat::Mp3:    return ma_encoding_format_mp3;
				case EncodedAudioFormat::Vorbis: return ma_encoding_format_vorbis;
			}

			ENGINE_CORE_ASSERT(false, "Unknown EncodedAudioFormat {}", std::to_underlying(format));
			return ma_encoding_format_unknown;
		}

		// The pages of an Ogg stream, which is complete when its pages tile the bytes exactly and the last one ends the
		// stream (the end-of-stream flag). stb_vorbis decodes up to the last complete page of a truncated file without an
		// error, so a file cut at a page boundary or inside a page is caught here. Errors: Parse with the byte offset.
		static Result<std::vector<OggPage>> ReadOggPages(std::span<const std::byte> bytes)
		{
			std::vector<OggPage> pages;
			size_t offset = 0;
			while (offset < bytes.size())
			{
				if (!HasPrefix(bytes, offset, "OggS"))
					return MakeError(ErrorCode::Parse, "corrupt Ogg file: no page starts at byte {}", offset);
				if (bytes.size() - offset < OggPageHeaderSize)
					return MakeError(ErrorCode::Parse, "truncated Ogg file: the page at byte {} is cut short", offset);
				const size_t segmentCount = ByteAt(bytes, offset + 26);
				if (bytes.size() - offset < OggPageHeaderSize + segmentCount)
					return MakeError(ErrorCode::Parse, "truncated Ogg file: the page at byte {} is cut short", offset);
				size_t bodySize = 0;
				for (size_t segment = 0; segment < segmentCount; ++segment)
					bodySize += ByteAt(bytes, offset + OggPageHeaderSize + segment);
				const size_t pageSize = OggPageHeaderSize + segmentCount + bodySize;
				if (bytes.size() - offset < pageSize)
				{
					return MakeError(ErrorCode::Parse, "truncated Ogg file: the page at byte {} needs {} bytes, {} are left", offset, pageSize,
						bytes.size() - offset);
				}
				pages.push_back({ .Flags = ByteAt(bytes, offset + 5),
					.Lacing = bytes.subspan(offset + OggPageHeaderSize, segmentCount),
					.Body = bytes.subspan(offset + OggPageHeaderSize + segmentCount, bodySize) });
				offset += pageSize;
			}
			if (pages.empty() || (pages.back().Flags & OggEndOfStream) == 0)
				return MakeError(ErrorCode::Parse, "truncated Ogg file: the last page does not end the stream");
			return pages;
		}

		// The header packet `name` that starts at `position`, as stb_vorbis reads it: its segments up to the first lacing
		// value below 255. A packet that starts on a new page needs a page without the continued-packet flag, and one that
		// runs onto the next page needs that flag there (stb_vorbis would otherwise read past the packet as padding). Moves
		// `position` past the packet. Errors: Parse for a packet that runs past the last page, breaks those flag rules or
		// crosses a page without segments.
		static Result<std::vector<std::byte>> ReadVorbisHeaderPacket(std::span<const OggPage> pages, OggPosition& position, std::string_view name)
		{
			std::vector<std::byte> packet;
			bool started = false;
			for (;;)
			{
				if (position.Page >= pages.size())
					return MakeError(ErrorCode::Parse, "truncated Ogg Vorbis file: the {} header runs past the last page", name);
				const OggPage& page = pages[position.Page];
				if (position.Segment == 0 && page.Lacing.empty())
					return MakeError(ErrorCode::Parse, "corrupt Ogg Vorbis file: a page without segments holds the {} header", name);
				if (position.Segment >= page.Lacing.size())
				{
					position = OggPosition{ .Page = position.Page + 1, .Segment = 0, .BodyOffset = 0 };
					if (position.Page < pages.size() && ((pages[position.Page].Flags & OggContinuedPacket) != 0) != started)
					{
						return MakeError(ErrorCode::Parse, "corrupt Ogg Vorbis file: the {} header {} a page {} the continued-packet flag", name,
							started ? "continues on" : "starts on", started ? "without" : "with");
					}
					continue;
				}
				const size_t size = std::to_integer<size_t>(page.Lacing[position.Segment]);
				const std::span<const std::byte> segment = page.Body.subspan(position.BodyOffset, size);
				packet.insert(packet.end(), segment.begin(), segment.end());
				position.BodyOffset += size;
				++position.Segment;
				started = true;
				if (size < 255)
					return packet;
			}
		}

		// The little-endian 32-bit value at `offset` of `packet`, advancing past it; nullopt when fewer than 4 bytes are left.
		static std::optional<uint32_t> ReadLittleEndian32(std::span<const std::byte> packet, size_t& offset)
		{
			if (packet.size() < offset || packet.size() - offset < 4)
				return std::nullopt;
			uint32_t value = 0;
			for (size_t index = 0; index < 4; ++index)
				value |= static_cast<uint32_t>(ByteAt(packet, offset + index)) << (8 * index);
			offset += 4;
			return value;
		}

		// The comment header (Vorbis I section 5): the vendor string, the comment count and each comment (a 32-bit length
		// and its bytes) inside the packet, then the framing bit. stb_vorbis allocates every length it reads before reading
		// the bytes, in int: a length past the packet is a huge or negative allocation, and a comment count past it wraps
		// its list's size into a heap overflow. Errors: Parse naming the field.
		// The vendor, comment-count and comment-length checks give those errors their precise messages; they are defense in
		// depth, not the guard. Each only moves `offset`, and every later field is read through ReadLittleEndian32, which
		// refuses a read past the packet, or the framing-bit check, which refuses an offset at or past its end. So any input
		// one of them refuses is also refused, with a less precise message, without it, and none of them is individually
		// necessary for safety: never remove the bounds-checked reads or the framing-bit check on the strength of these.
		static Status CheckVorbisComments(std::span<const std::byte> packet)
		{
			if (packet.size() < VorbisHeaderPrefixSize || ByteAt(packet, 0) != VorbisCommentPacket || !HasPrefix(packet, 1, "vorbis"))
				return MakeError(ErrorCode::Parse, "corrupt Ogg Vorbis file: the second header packet is not a comment header");
			size_t offset = VorbisHeaderPrefixSize;
			const std::optional<uint32_t> vendor = ReadLittleEndian32(packet, offset);
			if (!vendor.has_value() || *vendor > packet.size() - offset)
				return MakeError(ErrorCode::Parse, "corrupt Ogg Vorbis file: the comment header's vendor string runs past its packet");
			offset += *vendor;
			const std::optional<uint32_t> count = ReadLittleEndian32(packet, offset);
			if (!count.has_value() || *count > (packet.size() - offset) / 4)
			{
				return MakeError(ErrorCode::Parse, "corrupt Ogg Vorbis file: the comment header declares {} comments, more than its {}-byte packet holds",
					count.value_or(0), packet.size());
			}
			for (uint32_t comment = 0; comment < *count; ++comment)
			{
				const std::optional<uint32_t> length = ReadLittleEndian32(packet, offset);
				if (!length.has_value() || *length > packet.size() - offset)
					return MakeError(ErrorCode::Parse, "corrupt Ogg Vorbis file: comment {} of the comment header runs past its packet", comment);
				offset += *length;
			}
			if (offset >= packet.size() || (ByteAt(packet, offset) & 0x01) == 0)
				return MakeError(ErrorCode::Parse, "corrupt Ogg Vorbis file: the comment header has no framing bit");
			return {};
		}

		// base^exponent, saturated at 2^62.
		static uint64_t SaturatingPower(uint64_t base, uint32_t exponent)
		{
			constexpr uint64_t Limit = uint64_t{ 1 } << 62;
			uint64_t result = 1;
			for (uint32_t step = 0; step < exponent && result < Limit; ++step)
				result = base != 0 && result > Limit / base ? Limit : result * base;
			return result;
		}

		// The lookup values of a type 1 codebook: the largest r with r^dimensions <= entries (stb_vorbis's lookup1_values,
		// whose float estimate it corrects exactly). It converts (r0 + 1)^dimensions and r^dimensions to int, with r0 its
		// estimate (at most r + 1): when (r + 2)^dimensions exceeds INT_MAX the conversion is out of range and its result
		// differs between platforms, so such a codebook is refused. Errors: Parse.
		static Result<uint64_t> GetVorbisLookup1Values(uint32_t entries, uint32_t dimensions, uint32_t codebook)
		{
			if (entries == 0 || dimensions == 0)
				return uint64_t{ 0 };
			uint64_t low = 1; // 1^dimensions <= entries
			uint64_t high = entries;
			while (low < high)
			{
				const uint64_t middle = low + (high - low + 1) / 2;
				if (SaturatingPower(middle, dimensions) <= entries)
					low = middle;
				else
					high = middle - 1;
			}
			if (SaturatingPower(low + 2, dimensions) > static_cast<uint64_t>(INT_MAX))
			{
				return MakeError(ErrorCode::Parse, "unsupported Ogg Vorbis file: codebook {} has a type 1 lookup of {} entries in {} dimensions", codebook, entries,
					dimensions);
			}
			return low;
		}

		// The setup header's codebooks (Vorbis I section 3.2.1), read bit for bit as stb_vorbis reads them: each one's
		// entries times dimensions at most MaxVorbisCodebookValues, its codeword lengths and its lookup table, so the next
		// codebook is found where stb_vorbis finds it. stb_vorbis's own checks (sync pattern, codeword lengths, lookup types)
		// are refused here too. Errors: Parse naming the codebook.
		static Status CheckVorbisCodebooks(std::span<const std::byte> packet)
		{
			if (packet.size() < VorbisHeaderPrefixSize || ByteAt(packet, 0) != VorbisSetupPacket || !HasPrefix(packet, 1, "vorbis"))
				return MakeError(ErrorCode::Parse, "corrupt Ogg Vorbis file: the third header packet is not a setup header");
			VorbisBitReader bits(packet.subspan(VorbisHeaderPrefixSize));
			const uint32_t codebookCount = bits.Read(8) + 1;
			for (uint32_t codebook = 0; codebook < codebookCount; ++codebook)
			{
				if (bits.Read(8) != 0x42 || bits.Read(8) != 0x43 || bits.Read(8) != 0x56)
					return MakeError(ErrorCode::Parse, "corrupt Ogg Vorbis file: codebook {} of the setup header has no sync pattern", codebook);
				const uint32_t dimensionsLow = bits.Read(8);
				const uint32_t dimensions = (bits.Read(8) << 8) + dimensionsLow;
				const uint32_t entriesLow = bits.Read(8);
				const uint32_t entriesMiddle = bits.Read(8);
				const uint32_t entries = (bits.Read(8) << 16) + (entriesMiddle << 8) + entriesLow;
				const bool ordered = bits.Read(1) != 0;
				const bool sparse = !ordered && bits.Read(1) != 0;
				if (dimensions == 0 && entries != 0)
					return MakeError(ErrorCode::Parse, "corrupt Ogg Vorbis file: codebook {} has {} entries of no dimensions", codebook, entries);
				if (static_cast<uint64_t>(entries) * dimensions > MaxVorbisCodebookValues)
				{
					return MakeError(ErrorCode::Parse, "unsupported Ogg Vorbis file: codebook {} has {} entries of {} dimensions; at most {} values are supported",
						codebook, entries, dimensions, MaxVorbisCodebookValues);
				}

				if (ordered)
				{
					// Runs of entries of one codeword length, the length growing by one per run.
					uint32_t entry = 0;
					uint32_t length = bits.Read(5) + 1;
					while (entry < entries)
					{
						const uint32_t run = bits.Read(static_cast<uint32_t>(std::bit_width(entries - entry)));
						if (length >= 32 || run > entries - entry)
							return MakeError(ErrorCode::Parse, "corrupt Ogg Vorbis file: codebook {} has invalid ordered codeword lengths", codebook);
						entry += run;
						++length;
					}
				}
				else
				{
					for (uint32_t entry = 0; entry < entries && !bits.IsExhausted(); ++entry)
					{
						if (sparse && bits.Read(1) == 0)
							continue;
						if (bits.Read(5) + 1 == 32)
							return MakeError(ErrorCode::Parse, "corrupt Ogg Vorbis file: codebook {} has a 32-bit codeword length", codebook);
					}
				}

				const uint32_t lookupType = bits.Read(4);
				if (lookupType > 2)
					return MakeError(ErrorCode::Parse, "corrupt Ogg Vorbis file: codebook {} has lookup type {}", codebook, lookupType);
				if (lookupType == 0)
					continue;
				static_cast<void>(bits.Read(32)); // the minimum value
				static_cast<void>(bits.Read(32)); // the delta value
				const uint32_t valueBits = bits.Read(4) + 1;
				static_cast<void>(bits.Read(1)); // sequence_p
				uint64_t lookupValues = static_cast<uint64_t>(entries) * dimensions;
				if (lookupType == 1)
				{
					ENGINE_TRY_ASSIGN(lookupValues, GetVorbisLookup1Values(entries, dimensions, codebook));
				}
				if (lookupValues == 0)
					return MakeError(ErrorCode::Parse, "corrupt Ogg Vorbis file: codebook {} has an empty lookup table", codebook);
				for (uint64_t value = 0; value < lookupValues && !bits.IsExhausted(); ++value)
					static_cast<void>(bits.Read(valueBits));
			}
			return {};
		}

		// The header packets of an Ogg Vorbis file, before stb_vorbis reads them: the identification header alone on a first
		// page (stb_vorbis's own rule), then the comment header from the second page (CheckVorbisComments) and the setup
		// header after it (CheckVorbisCodebooks). Errors: Parse.
		static Status CheckVorbisHeaders(std::span<const OggPage> pages)
		{
			const OggPage& first = pages.front();
			const bool firstPage = (first.Flags & (OggFirstPage | OggContinuedPacket | OggEndOfStream)) == OggFirstPage;
			if (!firstPage || first.Lacing.size() != 1 || std::to_integer<size_t>(first.Lacing[0]) != VorbisIdentificationSize)
				return MakeError(ErrorCode::Parse, "corrupt Ogg Vorbis file: the first page does not hold the 30-byte identification header alone");
			OggPosition position{ .Page = 1, .Segment = 0, .BodyOffset = 0 };
			if (pages.size() > 1 && (pages[1].Flags & OggContinuedPacket) != 0)
				return MakeError(ErrorCode::Parse, "corrupt Ogg Vorbis file: the comment header starts on a page with the continued-packet flag");
			ENGINE_TRY_ASSIGN(const std::vector<std::byte> comments, ReadVorbisHeaderPacket(pages, position, "comment"));
			ENGINE_TRY(CheckVorbisComments(comments));
			ENGINE_TRY_ASSIGN(const std::vector<std::byte> setup, ReadVorbisHeaderPacket(pages, position, "setup"));
			return CheckVorbisCodebooks(setup);
		}

		// The checks of ProbeEncodedAudio and DecodeEncodedAudio; keeps the samples when `samples` is set.
		static Result<EncodedAudioInfo> Decode(std::span<const std::byte> bytes, std::vector<float>* samples)
		{
			ENGINE_TRY_ASSIGN(const EncodedAudioFormat format, DetectFormat(bytes));
			if (format == EncodedAudioFormat::Vorbis)
			{
				ENGINE_TRY_ASSIGN(const std::vector<OggPage> pages, ReadOggPages(bytes));
				ENGINE_TRY(CheckVorbisHeaders(pages));
			}

			ScopedDecoder decoder;
			if (const ma_result result = decoder.Initialize(bytes, ToMiniaudioFormat(format)); result != MA_SUCCESS)
				return MakeError(ErrorCode::Parse, "corrupt or truncated {} file: {}", EncodedAudioFormatToString(format), DescribeMiniaudioResult(result));

			ma_format sampleFormat = ma_format_unknown;
			ma_uint32 channels = 0;
			ma_uint32 sampleRate = 0;
			if (const ma_result result = ma_decoder_get_data_format(&decoder.Get(), &sampleFormat, &channels, &sampleRate, nullptr, 0);
				result != MA_SUCCESS)
			{
				return MakeError(ErrorCode::Parse, "corrupt {} file: {}", EncodedAudioFormatToString(format), DescribeMiniaudioResult(result));
			}
			if (channels != 1 && channels != 2)
			{
				return std::unexpected(Error(ErrorCode::Validation, std::format("the clip has {} channels; only mono and stereo clips are supported", channels))
						.WithHint("convert the file to mono or stereo"));
			}
			if (sampleRate < MinSampleRate || sampleRate > MaxSampleRate)
				return MakeError(ErrorCode::Validation, "the clip's sample rate of {} Hz is outside {} to {} Hz", sampleRate, MinSampleRate, MaxSampleRate);

			// The length a WAV or FLAC header states, which a complete file decodes in full; 0 when unknown. MP3 and Vorbis
			// lengths come from scanning the data itself (a truncated Ogg file was caught by its pages above).
			ma_uint64 declaredFrames = 0;
			const bool statesLength = format == EncodedAudioFormat::Wav || format == EncodedAudioFormat::Flac;
			if (!statesLength || ma_decoder_get_length_in_pcm_frames(&decoder.Get(), &declaredFrames) != MA_SUCCESS)
				declaredFrames = 0;

			std::vector<float> scratch;
			std::vector<float>& output = samples != nullptr ? *samples : scratch;
			output.clear();
			uint64_t frames = 0;
			for (;;)
			{
				const size_t offset = samples != nullptr ? static_cast<size_t>(frames) * channels : 0;
				output.resize(offset + static_cast<size_t>(DecodeChunkFrames) * channels);
				ma_uint64 read = 0;
				const ma_result result = ma_decoder_read_pcm_frames(&decoder.Get(), output.data() + offset, DecodeChunkFrames, &read);
				// Every sample the chunk decoded must be finite: an IEEE-float WAV can hold NaN or an infinity, and a 64-bit one
				// values beyond the range of f32, which the mixer would carry to the device and the capture buffer.
				const std::span<const float> decoded(output.data() + offset, static_cast<size_t>(read) * channels);
				const auto nonFinite = std::ranges::find_if(decoded, [](float sample)
				{
					return !std::isfinite(sample);
				});
				if (nonFinite != decoded.end())
				{
					const uint64_t frame = frames + static_cast<uint64_t>(std::distance(decoded.begin(), nonFinite)) / channels;
					return std::unexpected(Error(ErrorCode::Validation,
						std::format("the {} file holds a non-finite sample (NaN or infinity) at frame {}", EncodedAudioFormatToString(format), frame))
							.WithHint("re-export the file with finite samples, for example as 16-bit PCM"));
				}
				frames += read;
				if (result == MA_AT_END || (result == MA_SUCCESS && read == 0))
					break;
				if (result != MA_SUCCESS)
				{
					return MakeError(ErrorCode::Parse, "corrupt {} file: decoding failed at frame {}: {}", EncodedAudioFormatToString(format), frames,
						DescribeMiniaudioResult(result));
				}
			}
			output.resize(samples != nullptr ? static_cast<size_t>(frames) * channels : 0);

			if (declaredFrames != 0 && frames < declaredFrames)
			{
				return MakeError(ErrorCode::Parse, "truncated {} file: decoding ended at frame {} of {}", EncodedAudioFormatToString(format), frames,
					declaredFrames);
			}
			if (frames == 0)
				return MakeError(ErrorCode::Validation, "the {} file holds no audio frames", EncodedAudioFormatToString(format));
			return EncodedAudioInfo{ .Format = format, .SampleRate = sampleRate, .ChannelCount = channels, .FrameCount = frames };
		}

	}

	Result<EncodedAudioInfo> ProbeEncodedAudio(std::span<const std::byte> bytes)
	{
		return Utils::Decode(bytes, nullptr);
	}

	Result<DecodedAudio> DecodeEncodedAudio(std::span<const std::byte> bytes)
	{
		DecodedAudio decoded;
		ENGINE_TRY_ASSIGN(decoded.Info, Utils::Decode(bytes, &decoded.Samples));
		return decoded;
	}

	std::string_view EncodedAudioFormatToString(EncodedAudioFormat format)
	{
		switch (format)
		{
			case EncodedAudioFormat::Wav:    return "Wav";
			case EncodedAudioFormat::Flac:   return "Flac";
			case EncodedAudioFormat::Mp3:    return "Mp3";
			case EncodedAudioFormat::Vorbis: return "Vorbis";
		}

		ENGINE_CORE_ASSERT(false, "Unknown EncodedAudioFormat {}", std::to_underlying(format));
		return "Unknown";
	}

}
