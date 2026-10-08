#include "TestsPCH.h"

#include "Engine/AssetPipeline/Importers/AudioImporter.h"

#include "Engine/Asset/AudioClipData.h"
#include "Engine/Audio/AudioTypes.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Support/AssetTestFixture.h"
#include "Support/AudioTestData.h"
#include "Support/TestData.h"

#include <nlohmann/json.hpp>

#include <string>
#include <string_view>
#include <utility>

// The audio importer (Architecture §7.4: WAV, FLAC, MP3 and, approved in Docs/Decisions/0001-approvals.md, Ogg Vorbis). Its
// settings types are registered by the M12 contract; the import is a skipped skeleton (Docs/Decisions/0015-m12-decisions.md):
// stream B implements it, registers the importer in RegisterBuiltinImporters and removes the skips.

namespace Engine {

	namespace {

		// The bytes of Tests/Data/<relative>; fails the test case on error.
		Buffer ReadFixture(std::string_view relative)
		{
			Result<Buffer> bytes = FileSystem::ReadFile(Test::GetTestDataPath(relative));
			REQUIRE_MESSAGE(bytes.has_value(), bytes.error().ToString());
			return std::move(*bytes);
		}

		// Imports `bytes` as project://Assets/Audio/<name> with the stream mode `stream`.
		Result<ImportResult> ImportAudio(Test::AssetTestFixture& fixture, std::string_view name, std::span<const std::byte> bytes,
			std::string_view stream = "Auto")
		{
			const std::string relative = std::string("Assets/Audio/") + std::string(name);
			fixture.WriteProjectFile(relative, bytes);
			const Buffer source = fixture.ReadProjectFile(relative);
			Json settings = Json::object();
			settings["Stream"] = std::string(stream);
			AssetMetadata metadata;
			metadata.Handle = AssetHandle(0xa0d1000000000001ull);
			metadata.Type = AssetType::AudioClip;
			metadata.Importer = std::string(AudioImporter::Id);
			metadata.ImporterVersion = AudioImporter::Version;
			metadata.Settings = VariantValue(settings);
			ImportContext context({
				.Vfs = &fixture.GetVfs(),
				.SourcePath = fixture.ProjectPath(relative),
				.SourceBytes = source,
				.Settings = VariantValue(settings),
				.Registry = &fixture.GetRegistry(),
				.Assets = {},
				.EnvironmentBaker = nullptr,
				.ScriptDiagnostics = nullptr,
			});
			return AudioImporter().Import(context, metadata);
		}

		// The one clip an import cooked; fails the test case otherwise.
		AssetRef<AudioClipData> LoadClip(const ImportResult& result)
		{
			REQUIRE(result.Artifacts.size() == 1);
			REQUIRE(result.Artifacts.front().Type == AssetType::AudioClip);
			REQUIRE(result.Artifacts.front().SubAssetKey.empty());
			Result<AssetRef<AudioClipData>> clip = LoadCookedAudioClip(result.Artifacts.front().Cooked);
			REQUIRE_MESSAGE(clip.has_value(), clip.error().ToString());
			return std::move(*clip);
		}

	}

	TEST_SUITE("AssetPipeline")
	{
		TEST_CASE("AudioImporter: the settings types are registered with Auto as the default")
		{
			Test::AssetTestFixture fixture;
			const TypeRegistry& registry = fixture.GetRegistry();
			CHECK(registry.FindStruct("AudioImportSettings") != nullptr);
			CHECK(registry.FindEnum("AudioStreamMode") != nullptr);
			CHECK(AudioImportSettings{}.Stream == AudioStreamMode::Auto);
			const AudioImporter importer;
			CHECK(importer.GetId() == "Audio");
			CHECK(importer.GetMainType() == AssetType::AudioClip);
			CHECK(importer.GetSettingsTypeName() == "AudioImportSettings");
			CHECK(importer.CanImport(".wav"));
			CHECK(importer.CanImport(".FLAC"));
			CHECK(importer.CanImport(".mp3"));
			CHECK(importer.CanImport(".ogg"));
			CHECK_FALSE(importer.CanImport(".sfx"));
		}

		TEST_CASE("AudioImporter: WAV, FLAC, MP3 and Ogg Vorbis files import with their original bytes" * doctest::skip(true))
		{
			struct Fixture
			{
				std::string_view Name{};
				AudioClipEncoding Encoding = AudioClipEncoding::Wav;
			};
			const Fixture fixtures[] = {
				{ "Tone.wav", AudioClipEncoding::Wav },
				{ "Tone.flac", AudioClipEncoding::Flac },
				{ "Tone.mp3", AudioClipEncoding::Mp3 },
				{ "Tone.ogg", AudioClipEncoding::Vorbis },
			};
			for (const Fixture& entry : fixtures)
			{
				CAPTURE(std::string(entry.Name));
				Test::AssetTestFixture fixture;
				const Buffer bytes = ReadFixture(std::string("Assets/Audio/") + std::string(entry.Name));
				const Result<ImportResult> imported = ImportAudio(fixture, entry.Name, bytes);
				REQUIRE_MESSAGE(imported.has_value(), imported.error().ToString());
				const AssetRef<AudioClipData> clip = LoadClip(*imported);
				CHECK(clip->Encoding == entry.Encoding);
				CHECK(clip->Bytes == bytes);
				CHECK(clip->FrameCount > 0);
				CHECK_FALSE(clip->Stream);
				CHECK(imported->Dependencies.empty());
				CHECK(imported->Diagnostics.empty());
			}
		}

		TEST_CASE("AudioImporter: Auto streams clips longer than 10 seconds" * doctest::skip(true))
		{
			Test::AssetTestFixture fixture;
			const auto importTone = [&fixture](std::string_view name, uint64_t frames)
			{
				const Buffer wav = Test::MakeToneWav({ .SampleRate = 8000, .ChannelCount = 1, .FrameCount = frames, .Frequency = 440.0, .Amplitude = 0.5 });
				const Result<ImportResult> imported = ImportAudio(fixture, name, wav);
				REQUIRE_MESSAGE(imported.has_value(), imported.error().ToString());
				return LoadClip(*imported);
			};
			// Exactly 10 s decodes; a frame more streams (MaxDecodedClipSeconds).
			const AssetRef<AudioClipData> ten = importTone("Ten.wav", 80000);
			CHECK_FALSE(ten->Stream);
			const AssetRef<AudioClipData> longer = importTone("Longer.wav", 80001);
			CHECK(longer->Stream);
			CHECK(GetAudioClipDuration(*longer) > MaxDecodedClipSeconds);
		}

		TEST_CASE("AudioImporter: Stream and Decode override the length rule" * doctest::skip(true))
		{
			Test::AssetTestFixture fixture;
			const Buffer shortWav = Test::MakeToneWav({ .SampleRate = 48000, .ChannelCount = 1, .FrameCount = 4800 });
			const Result<ImportResult> streamed = ImportAudio(fixture, "Short.wav", shortWav, "Stream");
			REQUIRE_MESSAGE(streamed.has_value(), streamed.error().ToString());
			const AssetRef<AudioClipData> streamedClip = LoadClip(*streamed);
			CHECK(streamedClip->Stream);
			const Buffer longWav = Test::MakeToneWav({ .SampleRate = 8000, .ChannelCount = 1, .FrameCount = 8000 * 12 });
			const Result<ImportResult> decoded = ImportAudio(fixture, "Long.wav", longWav, "Decode");
			REQUIRE_MESSAGE(decoded.has_value(), decoded.error().ToString());
			const AssetRef<AudioClipData> decodedClip = LoadClip(*decoded);
			CHECK_FALSE(decodedClip->Stream);
		}

		TEST_CASE("AudioImporter: a file the decoder rejects is ImportFailed with the decoder's message" * doctest::skip(true))
		{
			Test::AssetTestFixture fixture;
			const Buffer garbage(256, std::byte{ 0x42 });
			const Result<ImportResult> rejected = ImportAudio(fixture, "Noise.wav", garbage);
			REQUIRE_FALSE(rejected.has_value());
			CHECK(rejected.error().GetCode() == ErrorCode::ImportFailed);
			CHECK(rejected.error().ToString().contains("Assets/Audio/Noise.wav"));
			const Buffer surround = Test::MakeToneWav({ .SampleRate = 48000, .ChannelCount = 6, .FrameCount = 480 });
			const Result<ImportResult> tooManyChannels = ImportAudio(fixture, "Surround.wav", surround);
			REQUIRE_FALSE(tooManyChannels.has_value());
			CHECK(tooManyChannels.error().GetCode() == ErrorCode::ImportFailed);
			CHECK(tooManyChannels.error().ToString().contains("mono or stereo"));
		}

		TEST_CASE("AudioImporter: the content decides the format, not the extension" * doctest::skip(true))
		{
			Test::AssetTestFixture fixture;
			const Buffer wav = Test::MakeToneWav({ .SampleRate = 48000, .ChannelCount = 1, .FrameCount = 4800 });
			const Result<ImportResult> imported = ImportAudio(fixture, "Disguised.mp3", wav);
			REQUIRE_MESSAGE(imported.has_value(), imported.error().ToString());
			const AssetRef<AudioClipData> clip = LoadClip(*imported);
			CHECK(clip->Encoding == AudioClipEncoding::Wav);
		}

		TEST_CASE("AudioImporter: importing twice gives identical artifacts" * doctest::skip(true))
		{
			Test::AssetTestFixture fixture;
			const Buffer wav = Test::MakeToneWav({ .SampleRate = 22050, .ChannelCount = 2, .FrameCount = 22050 });
			const Result<ImportResult> first = ImportAudio(fixture, "Twice.wav", wav);
			const Result<ImportResult> second = ImportAudio(fixture, "Twice.wav", wav);
			REQUIRE(first.has_value());
			REQUIRE(second.has_value());
			REQUIRE(first->Artifacts.size() == second->Artifacts.size());
			CHECK(first->Artifacts.front().Cooked == second->Artifacts.front().Cooked);
		}
	}

}
