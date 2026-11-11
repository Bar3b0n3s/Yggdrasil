#include "TestsPCH.h"

#include "Engine/Audio/AudioDecoder.h"

#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Hash.h"
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
#include <format>
#include <initializer_list>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Probing and decoding encoded audio (Architecture §7.4 AudioImporter, §10.1 clips that decode at registration;
// Docs/Decisions/0015-m12-decisions.md; Ogg Opus, Docs/Decisions/0034-m23-contract.md decision 16). The format fixtures
// are Tests/Data/Assets/Audio: Tone.wav and Tone.flac from their committed generator, Tone.mp3, Tone.ogg and Water.opus
// pinned with their licences in Tests/Data/LICENSES.md.

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

		// Rewrites the CRC of every whole page from the start of `bytes` (a page at each "OggS" whose header and body fit),
		// so a mutated page reaches the decoder instead of failing libogg's CRC check, as a crafted file's pages would;
		// stops at the first byte that does not start a whole page.
		void RepairOggCrcs(Buffer& bytes)
		{
			size_t offset = 0;
			while (bytes.size() - offset >= 27 && std::memcmp(bytes.data() + offset, "OggS", 4) == 0)
			{
				const size_t segments = std::to_integer<size_t>(bytes[offset + 26]);
				if (bytes.size() - offset < 27 + segments)
					return;
				size_t pageSize = 27 + segments;
				for (size_t segment = 0; segment < segments; ++segment)
					pageSize += std::to_integer<size_t>(bytes[offset + 27 + segment]);
				if (bytes.size() - offset < pageSize)
					return;
				std::fill_n(bytes.begin() + static_cast<std::ptrdiff_t>(offset + 22), 4, std::byte{ 0 });
				const uint32_t crc = Test::OggCrc(std::span<const std::byte>(bytes).subspan(offset, pageSize));
				for (size_t index = 0; index < 4; ++index)
					bytes[offset + 22 + index] = static_cast<std::byte>(crc >> (8 * index));
				offset += pageSize;
			}
		}

		// Water.opus's first three pages, the third (its first audio page: 13 packets of 20 ms) now ending the stream: an
		// Ogg Opus file of 12,168 frames, small enough to fuzz.
		std::vector<Test::OggTestPage> MakeShortOpusPages()
		{
			std::vector<Test::OggTestPage> pages = Test::SplitOggPages(ReadFixture("Assets/Audio/Water.opus"));
			REQUIRE(pages.size() > 3);
			pages.resize(3);
			pages.back().Flags |= Test::OggEndsStream;
			return pages;
		}

		// The lacing values of one packet of `size` bytes that ends on its page (RFC 3533): 255 for each full segment, then the
		// rest (0 when the size is a multiple of 255). At most 65,024 bytes, so it fits one page's 255 segments.
		std::vector<uint8_t> MakePacketLacing(size_t size)
		{
			REQUIRE(size <= 254 * 255 + 254);
			std::vector<uint8_t> lacing(size / 255, uint8_t{ 255 });
			lacing.push_back(static_cast<uint8_t>(size % 255));
			return lacing;
		}

		// `pages` with the identification header's channel count, channel mapping family and, for a family other than 0,
		// the stream count, coupled count and channel mapping (RFC 7845 section 5.1.1).
		Buffer WithOpusChannelMapping(std::vector<Test::OggTestPage> pages, uint8_t channels, uint8_t family, std::initializer_list<uint8_t> table)
		{
			Test::OggTestPage& head = pages.front();
			REQUIRE(head.Body.size() == 19);
			head.Body[9] = std::byte{ channels };
			head.Body[18] = std::byte{ family };
			for (const uint8_t value : table)
				head.Body.push_back(std::byte{ value });
			head.Lacing = { static_cast<uint8_t>(head.Body.size()) };
			return Test::JoinOggPages(pages);
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
			CHECK(EncodedAudioFormatToString(EncodedAudioFormat::Opus) == "Opus");
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

		// M23 (C23.1, A-OPUS; Docs/Decisions/0034-m23-contract.md decision 16, stream O): the pinned public-domain Ogg Opus
		// fixture (Water.opus, Tests/Data/LICENSES.md) decodes through libopus's fixed-point decoder to the same 48 kHz stereo
		// samples in every configuration: their XXH64 is the digest stream O committed from Debug and confirmed in Release
		// (Dist through FeatureTest's exported run).
		TEST_CASE("AudioDecoder: an Ogg Opus file decodes bit-exact to its committed digest in every configuration")
		{
			const Buffer bytes = ReadFixture("Assets/Audio/Water.opus");
			const Result<DecodedAudio> decoded = DecodeEncodedAudio(bytes);
			REQUIRE_MESSAGE(decoded.has_value(), decoded.error().ToString());
			CHECK(decoded->Info.Format == EncodedAudioFormat::Opus);
			CHECK(decoded->Info.SampleRate == 48000);
			CHECK(decoded->Info.ChannelCount == 2);
			// The last page's granule position (1,066,843) less the identification header's pre-skip (312).
			CHECK(decoded->Info.FrameCount == 1066531);
			CHECK(decoded->Samples.size() == decoded->Info.FrameCount * 2);
			const Result<EncodedAudioInfo> probed = ProbeEncodedAudio(bytes);
			REQUIRE(probed.has_value());
			CHECK(*probed == decoded->Info);
			const uint64_t digest = XXH64(std::as_bytes(std::span<const float>(decoded->Samples)));
			CHECK_MESSAGE(digest == 0x33F78152D7B526CDULL, std::format("digest {:016x}", digest));
		}

		TEST_CASE("AudioDecoder: an Ogg Opus file with channel mapping family 1 decodes as family 0 does")
		{
			// RFC 7845 section 5.1.1: family 1 (Vorbis channel order) with one coupled stream mapped to channels 0 and 1 is
			// the stereo layout family 0 implies.
			const std::vector<Test::OggTestPage> pages = MakeShortOpusPages();
			const Result<DecodedAudio> family0 = DecodeEncodedAudio(Test::JoinOggPages(pages));
			REQUIRE_MESSAGE(family0.has_value(), family0.error().ToString());
			CHECK(family0->Info.Format == EncodedAudioFormat::Opus);
			CHECK(family0->Info.FrameCount == 12168); // 13 packets of 960 frames less the 312-frame pre-skip
			const Result<DecodedAudio> family1 = DecodeEncodedAudio(WithOpusChannelMapping(pages, 2, 1, { 1, 1, 0, 1 }));
			REQUIRE_MESSAGE(family1.has_value(), family1.error().ToString());
			CHECK(family1->Info == family0->Info);
			CHECK(family1->Samples == family0->Samples);
		}

		TEST_CASE("AudioDecoder: other Ogg Opus channel layouts are Validation errors with the mono or stereo hint")
		{
			const std::vector<Test::OggTestPage> pages = MakeShortOpusPages();
			// Family 255 (no defined channel meaning), even with two channels.
			const Result<EncodedAudioInfo> unordered = ProbeEncodedAudio(WithOpusChannelMapping(pages, 2, 255, { 1, 1, 0, 1 }));
			REQUIRE_FALSE(unordered.has_value());
			CHECK(unordered.error().GetCode() == ErrorCode::Validation);
			CHECK(unordered.error().GetMessageText().contains("family 255"));
			CHECK(unordered.error().GetHint().contains("mono or stereo"));
			// Family 1 with six channels (5.1: four streams, two of them coupled).
			const Result<EncodedAudioInfo> surround = ProbeEncodedAudio(WithOpusChannelMapping(pages, 6, 1, { 4, 2, 0, 1, 2, 3, 4, 5 }));
			REQUIRE_FALSE(surround.has_value());
			CHECK(surround.error().GetCode() == ErrorCode::Validation);
			CHECK(surround.error().ToString().contains("mono or stereo"));
		}

		TEST_CASE("AudioDecoder: an Ogg Opus file that is not one logical stream is a Parse error")
		{
			// miniaudio's decoder sizes its buffers for a file's first link, so chained (a new stream after the first) and
			// multiplexed (interleaved streams) files are refused before opusfile reads them.
			const std::vector<Test::OggTestPage> pages = MakeShortOpusPages();
			REQUIRE(DecodeEncodedAudio(Test::JoinOggPages(pages)).has_value());
			std::vector<Test::OggTestPage> otherSerial = pages;
			otherSerial[2].Serial ^= 1;
			std::vector<Test::OggTestPage> secondStart = pages;
			secondStart[2].Flags |= Test::OggBeginsStream;
			std::vector<Test::OggTestPage> earlyEnd = pages;
			earlyEnd[1].Flags |= Test::OggEndsStream;
			std::vector<Test::OggTestPage> headerNotAlone = pages;
			headerNotAlone[0].Lacing.push_back(0);
			const std::pair<std::string_view, const std::vector<Test::OggTestPage>*> variants[] = {
				{ "another serial number", &otherSerial },
				{ "a second beginning", &secondStart },
				{ "an early end", &earlyEnd },
				{ "a second packet on the first page", &headerNotAlone },
			};
			for (const auto& [what, variant] : variants)
			{
				CAPTURE(std::string(what));
				const Result<EncodedAudioInfo> probed = ProbeEncodedAudio(Test::JoinOggPages(*variant));
				REQUIRE_FALSE(probed.has_value());
				CHECK(probed.error().GetCode() == ErrorCode::Parse);
			}
		}

		// RFC 7845 section 5.1: an identification header opusfile refuses (an unknown major version, no channels), behind the
		// stream checks it passes, is a Parse error whichever decoder miniaudio tries next.
		TEST_CASE("AudioDecoder: an Ogg Opus identification header libopus refuses is a Parse error")
		{
			const std::vector<Test::OggTestPage> pages = MakeShortOpusPages();
			std::vector<Test::OggTestPage> newerVersion = pages;
			newerVersion[0].Body[8] = std::byte{ 16 }; // major version 1 (the upper four bits): a later specification
			std::vector<Test::OggTestPage> noChannels = pages;
			noChannels[0].Body[9] = std::byte{ 0 };
			const std::pair<std::string_view, const std::vector<Test::OggTestPage>*> variants[] = {
				{ "version 16", &newerVersion },
				{ "no channels", &noChannels },
			};
			for (const auto& [what, variant] : variants)
			{
				CAPTURE(std::string(what));
				const Buffer bytes = Test::JoinOggPages(*variant);
				CHECK(GetErrorCode(ProbeEncodedAudio(bytes)) == ErrorCode::Parse);
				CHECK(GetErrorCode(DecodeEncodedAudio(bytes)) == ErrorCode::Parse);
			}
		}

		// An Ogg Opus file whose identification header opusfile refuses and whose later pages hold MP3 frames (Tone.mp3's), which
		// miniaudio's MP3 decoder, tried after the Opus backend, takes: the file is still Ogg Opus that libopus cannot open, a
		// Parse error, never a clip decoded as MP3 under the Opus encoding (MiniaudioOpusBackend::Decodes).
		TEST_CASE("AudioDecoder: an Ogg Opus file that another decoder would take is a Parse error")
		{
			std::vector<Test::OggTestPage> pages = MakeShortOpusPages();
			pages.resize(2); // the identification and comment headers
			pages[0].Body[8] = std::byte{ 16 };
			const Buffer mp3 = ReadFixture("Assets/Audio/Tone.mp3");
			constexpr size_t PageBodySize = 32000;
			for (size_t offset = 0; offset < mp3.size(); offset += PageBodySize)
			{
				Test::OggTestPage page;
				page.Serial = pages.front().Serial;
				page.Sequence = static_cast<uint32_t>(pages.size());
				page.Granule = uint64_t{ 960 } * pages.size();
				page.Body.assign(mp3.begin() + static_cast<std::ptrdiff_t>(offset), mp3.begin() + static_cast<std::ptrdiff_t>(std::min(offset + PageBodySize, mp3.size())));
				page.Lacing = MakePacketLacing(page.Body.size());
				pages.push_back(std::move(page));
			}
			pages.back().Flags |= Test::OggEndsStream;
			const Buffer bytes = Test::JoinOggPages(pages);
			CHECK(GetErrorCode(ProbeEncodedAudio(bytes)) == ErrorCode::Parse);
			CHECK(GetErrorCode(DecodeEncodedAudio(bytes)) == ErrorCode::Parse);
		}

		// An Ogg Opus file states its length in its last page's granule position (opusfile's op_pcm_total): one that decodes
		// fewer frames than that is truncated, a Parse error, as a WAV or FLAC file shorter than its header states is. Water.opus's
		// first four pages, the fourth ending the stream, decode in full; with that page's granule position raised by a second
		// they no longer do.
		TEST_CASE("AudioDecoder: an Ogg Opus file whose last granule position overstates its length is a Parse error")
		{
			std::vector<Test::OggTestPage> pages = Test::SplitOggPages(ReadFixture("Assets/Audio/Water.opus"));
			REQUIRE(pages.size() > 4);
			pages.resize(4);
			pages.back().Flags |= Test::OggEndsStream;
			const Result<EncodedAudioInfo> complete = ProbeEncodedAudio(Test::JoinOggPages(pages));
			REQUIRE_MESSAGE(complete.has_value(), complete.error().ToString());
			pages.back().Granule += 48000;
			CHECK(GetErrorCode(ProbeEncodedAudio(Test::JoinOggPages(pages))) == ErrorCode::Parse);
			CHECK(GetErrorCode(DecodeEncodedAudio(Test::JoinOggPages(pages))) == ErrorCode::Parse);
		}

		TEST_CASE("AudioDecoder: 200 seeded mutations of an Ogg Opus file never crash" * doctest::test_suite(Test::FuzzSuite))
		{
			// The seeds: Water.opus's first three pages (MakeShortOpusPages), as channel mapping families 0 and 1. Seven
			// mutations in eight then get their pages' CRCs rewritten (RepairOggCrcs), so the damage reaches opusfile's
			// header parsing and libopus's packet decoding as a crafted file's would; the eighth keeps the broken CRCs. A
			// clip the decoder accepts holds every promised sample, finite, at Opus's 48 kHz.
			const std::vector<Test::OggTestPage> pages = MakeShortOpusPages();
			const std::vector<Buffer> seeds = { Test::JoinOggPages(pages), WithOpusChannelMapping(pages, 2, 1, { 1, 1, 0, 1 }) };
			const Test::FuzzRun run = Test::RunFuzzCase(0xA0D10905, 200, [&seeds](Random& random, uint64_t /*index*/)
			{
				Buffer mutated = seeds[static_cast<size_t>(random.RangeInt(0, static_cast<int64_t>(seeds.size()) - 1))];
				Test::MutateBytes(mutated, random);
				if (random.RangeInt(0, 7) != 0)
					RepairOggCrcs(mutated);
				const Result<DecodedAudio> decoded = DecodeEncodedAudio(mutated);
				if (!decoded.has_value())
				{
					const ErrorCode code = decoded.error().GetCode();
					CHECK((code == ErrorCode::Parse || code == ErrorCode::Validation));
					return false;
				}
				const EncodedAudioInfo& info = decoded->Info;
				CHECK(info.SampleRate == 48000);
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
			{
				CHECK(run.Counts.Rejected > 0);
				CHECK(run.Counts.Accepted > 0);
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
			// The first page of an Ogg Speex stream: the 27-byte page header, one lacing value and the start of its "Speex   "
			// header packet. (Until M23 this case used Ogg Opus, which decodes since.)
			Buffer speex;
			const auto append = [&speex](std::string_view text)
			{
				for (const char character : text)
					speex.push_back(static_cast<std::byte>(character));
			};
			append("OggS");
			speex.insert(speex.end(), { std::byte{ 0 }, std::byte{ 2 } });  // version, beginning of stream
			speex.insert(speex.end(), 20, std::byte{ 0 });                  // granule position, serial, sequence, CRC
			speex.insert(speex.end(), { std::byte{ 1 }, std::byte{ 19 } }); // one segment of 19 bytes
			append("Speex   ");
			speex.insert(speex.end(), 11, std::byte{ 0 });
			const Result<EncodedAudioInfo> probed = ProbeEncodedAudio(speex);
			REQUIRE_FALSE(probed.has_value());
			CHECK(probed.error().GetCode() == ErrorCode::Parse);
			CHECK(probed.error().GetMessageText().contains("Speex"));
			CHECK(probed.error().GetHint().contains("Ogg Vorbis"));
			CHECK(probed.error().GetHint().contains("Ogg Opus"));
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
			// An Ogg Opus file cut inside a page, and one cut at a page boundary (its last page then does not end the
			// stream).
			const Buffer opus = Test::JoinOggPages(MakeShortOpusPages());
			Buffer cut = opus;
			cut.resize(opus.size() - 100);
			CHECK(GetErrorCode(ProbeEncodedAudio(cut)) == ErrorCode::Parse);
			std::vector<Test::OggTestPage> twoPages = MakeShortOpusPages();
			twoPages.pop_back();
			CHECK(GetErrorCode(ProbeEncodedAudio(Test::JoinOggPages(twoPages))) == ErrorCode::Parse);
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

			// Page 1 again with the longer setup header: its lacing values and its CRC (Test::OggCrc, over the page with its CRC
			// field zeroed).
			Buffer originalPage1(original.begin() + static_cast<std::ptrdiff_t>(page1), original.begin() + static_cast<std::ptrdiff_t>(page2));
			std::fill_n(originalPage1.begin() + 22, 4, std::byte{ 0 });
			REQUIRE(Test::OggCrc(originalPage1) == readU32(original, page1 + 22));
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
			writeU32(lookup, page1 + 22, Test::OggCrc(std::span<const std::byte>(lookup).subspan(page1)));
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
