#include "TestsPCH.h"

#include "Engine/Audio/AudioDecoder.h"

#include "Engine/Core/FileSystem.h"
#include "Support/AudioTestData.h"
#include "Support/Fuzz.h"
#include "Support/TestData.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Probing and decoding encoded audio (Architecture §7.4 AudioImporter, §10.1 clips that decode at registration;
// Docs/Decisions/0015-m12-decisions.md). The format fixtures are Tests/Data/Assets/Audio: Tone.wav and Tone.flac from their
// committed generator, Tone.mp3 and Tone.ogg pinned with their licences in Tests/Data/LICENSES.md.

namespace Engine {

	namespace {

		// The error code of a failed result; nullopt for a success.
		template<typename T>
		std::optional<ErrorCode> GetErrorCode(const Result<T>& result)
		{
			if (result.has_value())
				return std::nullopt;
			return result.error().GetCode();
		}

		// The bytes of Tests/Data/<relative>; fails the test case on error.
		Buffer ReadFixture(std::string_view relative)
		{
			Result<Buffer> bytes = FileSystem::ReadFile(Test::GetTestDataPath(relative));
			REQUIRE_MESSAGE(bytes.has_value(), bytes.error().ToString());
			return std::move(*bytes);
		}

	}

	TEST_SUITE("Audio")
	{
		TEST_CASE("AudioDecoder: the formats name themselves")
		{
			CHECK(EncodedAudioFormatToString(EncodedAudioFormat::Wav) == "Wav");
			CHECK(EncodedAudioFormatToString(EncodedAudioFormat::Flac) == "Flac");
			CHECK(EncodedAudioFormatToString(EncodedAudioFormat::Mp3) == "Mp3");
			CHECK(EncodedAudioFormatToString(EncodedAudioFormat::Vorbis) == "Vorbis");
			const EncodedAudioInfo info{ .Format = EncodedAudioFormat::Wav, .SampleRate = 48000, .ChannelCount = 1, .FrameCount = 24000 };
			CHECK(info.GetDurationSeconds() == doctest::Approx(0.5));
			CHECK(EncodedAudioInfo{}.GetDurationSeconds() == 0.0);
		}

		TEST_CASE("AudioDecoder: 200 seeded mutations of WAV, FLAC, MP3 and Ogg Vorbis files never crash" * doctest::test_suite(Test::FuzzSuite))
		{
			// The seeds: the four format fixtures of Tests/Data/Assets/Audio and an IEEE-float WAV. The MP3, the largest, seeds
			// one iteration in eight. A clip the decoder accepts holds every promised sample, finite, in the accepted ranges.
			const std::array<float, 8> floats = { 0.0f, 0.5f, -0.5f, 1.0f, -1.0f, 0.25f, -0.25f, 0.0f };
			const std::vector<Buffer> seeds = { ReadFixture("Assets/Audio/Tone.wav"), ReadFixture("Assets/Audio/Tone.flac"),
				ReadFixture("Assets/Audio/Tone.ogg"), Test::MakeFloatWav(floats, 48000, 2), ReadFixture("Assets/Audio/Tone.mp3") };
			const Test::FuzzRun run = Test::RunFuzzCase(0xA0D10, 200, [&seeds](Random& random, uint64_t /*index*/)
			{
				const size_t seed = random.RangeInt(0, 7) == 0 ? seeds.size() - 1 : static_cast<size_t>(random.RangeInt(0, static_cast<int64_t>(seeds.size()) - 2));
				Buffer mutated = seeds[seed];
				Test::MutateBytes(mutated, random);
				const Result<DecodedAudio> decoded = DecodeEncodedAudio(mutated);
				if (!decoded.has_value())
				{
					const ErrorCode code = decoded.error().GetCode();
					CHECK((code == ErrorCode::Parse || code == ErrorCode::Validation));
					return false;
				}
				const EncodedAudioInfo& info = decoded->Info;
				CHECK(info.SampleRate >= 8000);
				CHECK(info.SampleRate <= 192000);
				CHECK((info.ChannelCount == 1 || info.ChannelCount == 2));
				CHECK(info.FrameCount > 0);
				CHECK(decoded->Samples.size() == info.FrameCount * info.ChannelCount);
				CHECK(std::ranges::all_of(decoded->Samples, [](float sample)
				{
					return std::isfinite(sample);
				}));
				return true;
			});
			if (Test::IsFuzzSample(run.Range))
				CHECK(run.Counts.Rejected > 0);
		}

		TEST_CASE("AudioDecoder: a WAV file is probed with its exact frame count")
		{
			const Test::TestToneSpecification tone{ .SampleRate = 44100, .ChannelCount = 2, .FrameCount = 12345, .Frequency = 440.0, .Amplitude = 0.5 };
			const Result<EncodedAudioInfo> info = ProbeEncodedAudio(Test::MakeToneWav(tone));
			REQUIRE_MESSAGE(info.has_value(), info.error().ToString());
			CHECK(info->Format == EncodedAudioFormat::Wav);
			CHECK(info->SampleRate == 44100);
			CHECK(info->ChannelCount == 2);
			CHECK(info->FrameCount == 12345);
		}

		TEST_CASE("AudioDecoder: decoding a WAV file returns its samples")
		{
			const Test::TestToneSpecification tone{ .SampleRate = 48000, .ChannelCount = 1, .FrameCount = 480, .Frequency = 1000.0, .Amplitude = 0.5 };
			const Result<DecodedAudio> decoded = DecodeEncodedAudio(Test::MakeToneWav(tone));
			REQUIRE_MESSAGE(decoded.has_value(), decoded.error().ToString());
			const std::vector<int16_t> expected = Test::MakeTonePcm16(tone);
			REQUIRE(decoded->Samples.size() == expected.size());
			for (size_t index = 0; index < expected.size(); ++index)
				CHECK(decoded->Samples[index] == doctest::Approx(static_cast<float>(expected[index]) / 32768.0f).epsilon(0.0001));
		}

		TEST_CASE("AudioDecoder: WAV, FLAC, MP3 and Ogg Vorbis fixtures decode")
		{
			struct Fixture
			{
				std::string_view Path{};
				EncodedAudioFormat Format = EncodedAudioFormat::Wav;
			};
			const Fixture fixtures[] = {
				{ "Assets/Audio/Tone.wav", EncodedAudioFormat::Wav },
				{ "Assets/Audio/Tone.flac", EncodedAudioFormat::Flac },
				{ "Assets/Audio/Tone.mp3", EncodedAudioFormat::Mp3 },
				{ "Assets/Audio/Tone.ogg", EncodedAudioFormat::Vorbis },
			};
			for (const Fixture& fixture : fixtures)
			{
				CAPTURE(std::string(fixture.Path));
				const Result<DecodedAudio> decoded = DecodeEncodedAudio(ReadFixture(fixture.Path));
				REQUIRE_MESSAGE(decoded.has_value(), decoded.error().ToString());
				CHECK(decoded->Info.Format == fixture.Format);
				CHECK(decoded->Info.FrameCount > 0);
				CHECK((decoded->Info.ChannelCount == 1 || decoded->Info.ChannelCount == 2));
				CHECK(decoded->Samples.size() == decoded->Info.FrameCount * decoded->Info.ChannelCount);
				const Result<EncodedAudioInfo> probed = ProbeEncodedAudio(ReadFixture(fixture.Path));
				REQUIRE(probed.has_value());
				CHECK(*probed == decoded->Info);
			}
		}

		TEST_CASE("AudioDecoder: bytes that are no audio file are a Parse error naming the formats")
		{
			const Buffer text = { std::byte{ 'h' }, std::byte{ 'e' }, std::byte{ 'l' }, std::byte{ 'l' }, std::byte{ 'o' } };
			const Result<EncodedAudioInfo> probed = ProbeEncodedAudio(text);
			REQUIRE_FALSE(probed.has_value());
			CHECK(probed.error().GetCode() == ErrorCode::Parse);
			const std::string message = probed.error().ToString();
			CHECK(message.contains("WAV"));
			CHECK(message.contains("Ogg Vorbis"));
			CHECK(GetErrorCode(ProbeEncodedAudio({})) == ErrorCode::Parse);
		}

		TEST_CASE("AudioDecoder: an Ogg file of another codec is a Parse error with a conversion hint")
		{
			// The first page of an Ogg Opus stream: the 27-byte page header, one lacing value and the "OpusHead" packet.
			Buffer opus;
			const auto append = [&opus](std::string_view text)
			{
				for (const char character : text)
					opus.push_back(static_cast<std::byte>(character));
			};
			append("OggS");
			opus.insert(opus.end(), { std::byte{ 0 }, std::byte{ 2 } });  // version, beginning of stream
			opus.insert(opus.end(), 20, std::byte{ 0 });                  // granule position, serial, sequence, CRC
			opus.insert(opus.end(), { std::byte{ 1 }, std::byte{ 19 } }); // one segment of 19 bytes
			append("OpusHead");
			opus.insert(opus.end(), { std::byte{ 1 }, std::byte{ 2 } }); // version, channels
			opus.insert(opus.end(), 9, std::byte{ 0 });
			const Result<EncodedAudioInfo> probed = ProbeEncodedAudio(opus);
			REQUIRE_FALSE(probed.has_value());
			CHECK(probed.error().GetCode() == ErrorCode::Parse);
			CHECK(probed.error().GetMessageText().contains("Opus"));
			CHECK(probed.error().GetHint().contains("Ogg Vorbis"));
		}

		TEST_CASE("AudioDecoder: a truncated file fails the probe")
		{
			const Test::TestToneSpecification tone{ .SampleRate = 48000, .ChannelCount = 1, .FrameCount = 4800, .Frequency = 440.0, .Amplitude = 0.5 };
			Buffer wav = Test::MakeToneWav(tone);
			wav.resize(30);
			CHECK(GetErrorCode(ProbeEncodedAudio(wav)) == ErrorCode::Parse);
			Buffer ogg = ReadFixture("Assets/Audio/Tone.ogg");
			ogg.resize(ogg.size() / 2);
			CHECK(GetErrorCode(ProbeEncodedAudio(ogg)) == ErrorCode::Parse);
		}

		TEST_CASE("AudioDecoder: Ogg Vorbis headers stb_vorbis would overflow or over-allocate on are Parse errors")
		{
			// Found by the AudioDecoder fuzz case under AddressSanitizer (Docs/Decisions/0024-m15-contract.md decision 13):
			// stb_vorbis allocates each comment header length before reading its bytes, in int, and sizes its codebook tables
			// from entries times dimensions. Each damaged Tone.ogg below reached stb_vorbis before the header checks.
			const Buffer original = ReadFixture("Assets/Audio/Tone.ogg");
			REQUIRE(DecodeEncodedAudio(original).has_value());
			const auto readU32 = [](std::span<const std::byte> bytes, size_t offset)
			{
				uint32_t value = 0;
				for (size_t index = 0; index < 4; ++index)
					value |= std::to_integer<uint32_t>(bytes[offset + index]) << (8 * index);
				return value;
			};
			const auto writeBytes = [](Buffer& bytes, size_t offset, std::initializer_list<uint8_t> values)
			{
				std::ranges::transform(values, bytes.begin() + static_cast<std::ptrdiff_t>(offset), [](uint8_t value)
				{
					return std::byte{ value };
				});
			};
			const auto writeU32 = [&writeBytes](Buffer& bytes, size_t offset, uint32_t value)
			{
				writeBytes(bytes, offset,
					{ static_cast<uint8_t>(value), static_cast<uint8_t>(value >> 8), static_cast<uint8_t>(value >> 16), static_cast<uint8_t>(value >> 24) });
			};
			const auto expectParseError = [](std::string_view what, std::span<const std::byte> bytes, std::string_view message)
			{
				CAPTURE(std::string(what));
				const Result<DecodedAudio> decoded = DecodeEncodedAudio(bytes);
				REQUIRE_FALSE(decoded.has_value());
				CHECK(decoded.error().GetCode() == ErrorCode::Parse);
				CHECK_MESSAGE(decoded.error().GetMessageText().contains(message), decoded.error().ToString());
			};

			// Tone.ogg: page 0 (58 bytes) holds the identification header; page 1 (14 lacing values) holds the 66-byte comment
			// header at byte 99 (a 29-byte vendor string, then one comment) and then the setup header, whose codebook 0 has the
			// sync pattern 42 43 56, one dimension and 64 entries (least significant byte first).
			const size_t comment = 58 + 27 + 14;
			const size_t setup = comment + 66;
			REQUIRE(std::memcmp(original.data() + comment, "\x03vorbis", 7) == 0);
			REQUIRE(readU32(original, comment + 7) == 29);
			REQUIRE(readU32(original, comment + 40) == 1);
			REQUIRE(std::memcmp(original.data() + setup, "\x05vorbis", 7) == 0);
			REQUIRE(std::memcmp(original.data() + setup + 8, "\x42\x43\x56\x01\x00\x40\x00\x00", 8) == 0);

			// The vendor, comment-count and comment-length cases check the messages of checks that are defense in depth behind
			// the bounds-checked reads and the framing bit (CheckVorbisComments).
			// A vendor string longer than its packet: a negative int in stb_vorbis, then a huge allocation.
			Buffer vendor = original;
			writeU32(vendor, comment + 7, 0xFFFFFFF0);
			expectParseError("vendor", vendor, "vendor string");
			// A comment count whose list stb_vorbis sizes as 8 x 0x20000001 bytes, 8 in int: a heap overflow.
			Buffer count = original;
			writeU32(count, comment + 40, 0x20000001);
			expectParseError("comment count", count, "536870913 comments");
			// A comment longer than its packet.
			Buffer length = original;
			writeU32(length, comment + 44, 0x7FFFFFFF);
			expectParseError("comment length", length, "comment 0");
			// Bits written least significant first, as the setup header packs them (Vorbis I section 2.1.4).
			const auto writeBits = [](std::vector<uint8_t>& bits, uint64_t value, uint32_t count)
			{
				for (uint32_t bit = 0; bit < count; ++bit)
					bits.push_back(static_cast<uint8_t>((value >> bit) & 1));
			};
			const auto packBits = [](std::span<const uint8_t> bits)
			{
				Buffer bytes((bits.size() + 7) / 8, std::byte{ 0 });
				for (size_t bit = 0; bit < bits.size(); ++bit)
					bytes[bit / 8] |= static_cast<std::byte>(bits[bit] << (bit % 8));
				return bytes;
			};

			// A file of the original identification and comment headers and a setup header of one codebook of 16385 entries in
			// 65535 dimensions (1,073,790,975 values) with ordered 15-bit codeword lengths and a type 2 lookup of 1-bit values:
			// every structure stb_vorbis reads before its lookup table is valid, so it reaches the table, whose allocation of
			// 2 bytes per value (setup_temp_malloc's int parameter) overflows into a negative size, an allocation-size-too-big
			// abort under AddressSanitizer. The values cap refuses the codebook first.
			std::vector<uint8_t> capBits;
			writeBits(capBits, 0, 8); // one codebook
			writeBits(capBits, 0x564342, 24);
			writeBits(capBits, 65535, 16); // dimensions
			writeBits(capBits, 16385, 24); // entries
			writeBits(capBits, 1, 1);      // ordered
			writeBits(capBits, 14, 5);     // codeword length 15
			writeBits(capBits, 16385, 15); // one run of every entry (the run's width: bit_width(16385))
			writeBits(capBits, 2, 4);      // lookup type 2
			writeBits(capBits, 0, 32 + 32 + 4 + 1 + 64);
			const Buffer capSetup = packBits(capBits);
			Buffer cap(original.begin(), original.begin() + 58);
			for (const char value : std::string_view("OggS"))
				cap.push_back(static_cast<std::byte>(value));
			cap.insert(cap.end(), { std::byte{ 0 }, std::byte{ 0x04 } }); // version, end of stream
			cap.insert(cap.end(), 20, std::byte{ 0 });                    // granule position, serial, sequence, CRC
			cap.insert(cap.end(), { std::byte{ 2 }, std::byte{ 66 }, static_cast<std::byte>(7 + capSetup.size()) });
			cap.insert(cap.end(), original.begin() + static_cast<std::ptrdiff_t>(comment), original.begin() + static_cast<std::ptrdiff_t>(setup + 7));
			cap.insert(cap.end(), capSetup.begin(), capSetup.end());
			expectParseError("codebook values", cap, "codebook 0 has 16385 entries of 65535 dimensions; at most 1048576 values are supported");

			// Tone.ogg with one more codebook after its 35: 32 dimensions, one entry with a 1-bit codeword and a type 1 lookup
			// of 1-bit values, followed by two multiplicands. Its lookup has 1 value (1^32 <= 1 < 2^32), but stb_vorbis's
			// lookup1_values converts 2^32 to int, which x86-64 makes INT_MIN and AArch64 INT_MAX: on x86-64 it computes 2
			// values, reads both multiplicands and decodes the file; elsewhere it reads one and fails on the setup header.
			// Refused on every platform, before stb_vorbis reads it.
			const size_t page1 = 58;
			REQUIRE(std::memcmp(original.data() + page1, "OggS", 4) == 0);
			const size_t segmentCount = std::to_integer<size_t>(original[page1 + 26]);
			REQUIRE(page1 + 27 + segmentCount == comment);
			REQUIRE(std::to_integer<size_t>(original[page1 + 27]) == 66);
			size_t setupSize = 0;
			for (size_t segment = 1; segment < segmentCount; ++segment)
				setupSize += std::to_integer<size_t>(original[page1 + 27 + segment]);
			REQUIRE(std::to_integer<size_t>(original[page1 + 27 + segmentCount - 1]) < 255); // the setup header ends on page 1
			const size_t page2 = setup + setupSize;
			REQUIRE(std::memcmp(original.data() + page2, "OggS", 4) == 0);

			// The setup header's bits after its 7-byte prefix, and the bit where its codebooks end (the time-domain section),
			// found by reading every codebook as CheckVorbisCodebooks and stb_vorbis read it.
			std::vector<uint8_t> setupBits;
			for (size_t byte = setup + 7; byte < page2; ++byte)
				writeBits(setupBits, std::to_integer<uint8_t>(original[byte]), 8);
			size_t position = 0;
			const auto readBits = [&setupBits, &position](uint64_t count)
			{
				uint64_t value = 0;
				for (uint64_t bit = 0; bit < count; ++bit, ++position)
				{
					if (position < setupBits.size() && bit < 64)
						value |= static_cast<uint64_t>(setupBits[position]) << bit;
				}
				return value;
			};
			const uint64_t codebookCount = readBits(8) + 1;
			REQUIRE(codebookCount == 35);
			for (uint64_t codebook = 0; codebook < codebookCount; ++codebook)
			{
				CAPTURE(codebook);
				REQUIRE(readBits(24) == 0x564342);
				const uint64_t dimensions = readBits(16);
				const uint64_t entries = readBits(24);
				if (readBits(1) != 0)
				{
					static_cast<void>(readBits(5));
					for (uint64_t entry = 0; entry < entries;)
						entry += readBits(static_cast<uint64_t>(std::bit_width(entries - entry)));
				}
				else
				{
					const bool sparse = readBits(1) != 0;
					for (uint64_t entry = 0; entry < entries; ++entry)
					{
						if (!sparse || readBits(1) != 0)
							static_cast<void>(readBits(5));
					}
				}
				const uint64_t lookupType = readBits(4);
				REQUIRE(lookupType <= 2);
				if (lookupType == 0)
					continue;
				static_cast<void>(readBits(32 + 32));
				const uint64_t valueBits = readBits(4) + 1;
				static_cast<void>(readBits(1));
				uint64_t values = entries * dimensions;
				if (lookupType == 1)
				{
					// The largest r with r^dimensions <= entries (Tone.ogg's codebooks are small).
					const auto power = [dimensions](uint64_t base)
					{
						uint64_t result = 1;
						for (uint64_t step = 0; step < dimensions; ++step)
							result *= base;
						return result;
					};
					values = 1;
					while (power(values + 1) <= entries)
						++values;
				}
				static_cast<void>(readBits(values * valueBits));
			}
			const size_t codebooksEnd = position;
			// The time-domain section: one transform of type 0.
			REQUIRE(readBits(6) == 0);
			REQUIRE(readBits(16) == 0);

			std::vector<uint8_t> extended;
			writeBits(extended, codebookCount, 8); // one more codebook
			extended.insert(extended.end(), setupBits.begin() + 8, setupBits.begin() + static_cast<std::ptrdiff_t>(codebooksEnd));
			writeBits(extended, 0x564342, 24);
			writeBits(extended, 32, 16); // dimensions
			writeBits(extended, 1, 24);  // entries
			writeBits(extended, 0, 2);   // not ordered, not sparse
			writeBits(extended, 0, 5);   // a codeword length of 1
			writeBits(extended, 1, 4);   // lookup type 1
			writeBits(extended, 0, 32 + 32 + 4 + 1);
			writeBits(extended, 0, 2); // two 1-bit multiplicands
			extended.insert(extended.end(), setupBits.begin() + static_cast<std::ptrdiff_t>(codebooksEnd), setupBits.end());
			const Buffer extendedSetup = packBits(extended);

			// Page 1 again with the longer setup header: its lacing values and its CRC (Ogg's CRC-32: polynomial 0x04C11DB7,
			// no reflection, initial value and final XOR 0, over the page with its CRC field zeroed).
			const auto oggCrc = [](std::span<const std::byte> page)
			{
				uint32_t crc = 0;
				for (const std::byte value : page)
				{
					crc ^= std::to_integer<uint32_t>(value) << 24;
					for (int bit = 0; bit < 8; ++bit)
						crc = (crc & 0x80000000u) != 0 ? (crc << 1) ^ 0x04C11DB7u : crc << 1;
				}
				return crc;
			};
			Buffer originalPage1(original.begin() + static_cast<std::ptrdiff_t>(page1), original.begin() + static_cast<std::ptrdiff_t>(page2));
			std::fill_n(originalPage1.begin() + 22, 4, std::byte{ 0 });
			REQUIRE(oggCrc(originalPage1) == readU32(original, page1 + 22));
			std::vector<std::byte> lacing = { std::byte{ 66 } };
			for (size_t left = 7 + extendedSetup.size();; left -= 255)
			{
				lacing.push_back(static_cast<std::byte>(std::min<size_t>(left, 255)));
				if (left < 255)
					break;
			}
			Buffer lookup(original.begin(), original.begin() + static_cast<std::ptrdiff_t>(page1 + 26));
			lookup.push_back(static_cast<std::byte>(lacing.size()));
			lookup.insert(lookup.end(), lacing.begin(), lacing.end());
			lookup.insert(lookup.end(), original.begin() + static_cast<std::ptrdiff_t>(comment), original.begin() + static_cast<std::ptrdiff_t>(setup + 7));
			lookup.insert(lookup.end(), extendedSetup.begin(), extendedSetup.end());
			std::fill_n(lookup.begin() + static_cast<std::ptrdiff_t>(page1 + 22), 4, std::byte{ 0 });
			writeU32(lookup, page1 + 22, oggCrc(std::span<const std::byte>(lookup).subspan(page1)));
			lookup.insert(lookup.end(), original.begin() + static_cast<std::ptrdiff_t>(page2), original.end());
			expectParseError("type 1 lookup", lookup, "codebook 35 has a type 1 lookup of 1 entries in 32 dimensions");
		}

		TEST_CASE("AudioDecoder: a float WAV file with a non-finite sample is a Validation error naming the frame")
		{
			// An IEEE-float WAV can hold NaN and infinities, which the mixer would carry to the device and the capture buffer.
			// The probe decodes every frame, so the importer refuses such a file whether the clip will stream or not.
			std::vector<float> samples(4800 * 2, 0.25f);
			const Buffer finite = Test::MakeFloatWav(samples, 48000, 2);
			const Result<DecodedAudio> decoded = DecodeEncodedAudio(finite);
			REQUIRE_MESSAGE(decoded.has_value(), decoded.error().ToString());
			CHECK(decoded->Samples == samples);

			const float infinity = std::numeric_limits<float>::infinity();
			for (const float bad : { std::numeric_limits<float>::quiet_NaN(), infinity, -infinity })
			{
				CAPTURE(bad);
				std::vector<float> withBad = samples;
				withBad[4500 * 2 + 1] = bad; // frame 4500, in the second decode chunk
				const Buffer wav = Test::MakeFloatWav(withBad, 48000, 2);
				const Result<EncodedAudioInfo> probed = ProbeEncodedAudio(wav);
				REQUIRE_FALSE(probed.has_value());
				CHECK(probed.error().GetCode() == ErrorCode::Validation);
				CHECK(probed.error().GetMessageText().contains("non-finite"));
				CHECK(probed.error().GetMessageText().contains("frame 4500"));
				CHECK_FALSE(probed.error().GetHint().empty());
				CHECK(GetErrorCode(DecodeEncodedAudio(wav)) == ErrorCode::Validation);
			}
		}

		TEST_CASE("AudioDecoder: more than two channels, unsupported rates and empty files are Validation errors")
		{
			const Result<EncodedAudioInfo> surround = ProbeEncodedAudio(
				Test::MakeToneWav({ .SampleRate = 48000, .ChannelCount = 6, .FrameCount = 480, .Frequency = 440.0, .Amplitude = 0.5 }));
			REQUIRE_FALSE(surround.has_value());
			CHECK(surround.error().GetCode() == ErrorCode::Validation);
			CHECK(surround.error().ToString().contains("mono or stereo"));
			CHECK(GetErrorCode(ProbeEncodedAudio(Test::MakeToneWav({ .SampleRate = 4000, .ChannelCount = 1, .FrameCount = 480 })))
				== ErrorCode::Validation);
			CHECK(GetErrorCode(ProbeEncodedAudio(Test::MakeToneWav({ .SampleRate = 48000, .ChannelCount = 1, .FrameCount = 0 })))
				== ErrorCode::Validation);
		}
	}

}
