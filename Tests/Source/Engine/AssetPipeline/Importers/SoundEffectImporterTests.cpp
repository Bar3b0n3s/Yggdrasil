#include "TestsPCH.h"

#include "Engine/AssetPipeline/Importers/SoundEffectImporter.h"

#include "Engine/Asset/AudioClipData.h"
#include "Engine/Asset/BuiltinAssets.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Support/AssetTestFixture.h"
#include "Support/TestData.h"

#include <nlohmann/json.hpp>

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

// The .sfx sound effect (Architecture §6.6, §7.4, §10.3). Its reflected types are registered by the M12 contract; the
// document functions and the import are skipped skeletons (Docs/Decisions/0015-m12-decisions.md): stream B implements them,
// registers the importer in RegisterBuiltinImporters, adds the ten presets (Resources/Audio/*.sfx and their
// EngineAssets.json entries) and removes the skips.

namespace Engine {

	namespace {

		// §6.6's example, verbatim.
		constexpr std::string_view LineClearText = R"({
	"Format": "SoundEffect", "Version": 1, "Seed": 7, "Volume": 0.8,
	"Layers": [
		{ "Wave": "Square", "DutyCycle": 0.5, "Notes": ["C5:0.06", "E5:0.06", "G5:0.06", "C6:0.18"],
		  "Envelope": { "Attack": 0.005, "Decay": 0.05, "Sustain": 0.6, "Release": 0.08 } },
		{ "Wave": "Noise", "Duration": 0.05, "StartFrequency": 4000, "EndFrequency": 800, "Volume": 0.3,
		  "Envelope": { "Attack": 0.0, "Decay": 0.05, "Sustain": 0.0, "Release": 0.0 } }
	]
})";

		// The error code of a failed result; nullopt for a success.
		template<typename T>
		std::optional<ErrorCode> GetErrorCode(const Result<T>& result)
		{
			if (result.has_value())
				return std::nullopt;
			return result.error().GetCode();
		}

		// Imports `text` as project://Assets/Audio/<name>.
		Result<ImportResult> ImportSoundEffect(Test::AssetTestFixture& fixture, std::string_view name, std::string_view text)
		{
			const std::string relative = std::string("Assets/Audio/") + std::string(name);
			fixture.WriteProjectText(relative, text);
			const Buffer source = fixture.ReadProjectFile(relative);
			AssetMetadata metadata;
			metadata.Handle = AssetHandle(0x5f00000000000001ull);
			metadata.Type = AssetType::AudioClip;
			metadata.Importer = std::string(SoundEffectImporter::Id);
			metadata.ImporterVersion = SoundEffectImporter::Version;
			ImportContext context({
				.Vfs = &fixture.GetVfs(),
				.SourcePath = fixture.ProjectPath(relative),
				.SourceBytes = source,
				.Settings = VariantValue(),
				.Registry = &fixture.GetRegistry(),
				.Assets = {},
				.EnvironmentBaker = nullptr,
				.ScriptDiagnostics = nullptr,
			});
			return SoundEffectImporter().Import(context, metadata);
		}

	}

	TEST_SUITE("AssetPipeline")
	{
		TEST_CASE("SoundEffectImporter: the description types are registered with their defaults")
		{
			Test::AssetTestFixture fixture;
			const TypeRegistry& registry = fixture.GetRegistry();
			CHECK(registry.FindEnum("SoundWave") != nullptr);
			CHECK(registry.FindStruct("SoundEnvelope") != nullptr);
			CHECK(registry.FindStruct("SoundLayer") != nullptr);
			CHECK(registry.FindStruct("SoundEffect") != nullptr);
			const SoundEffectImporter importer;
			CHECK(importer.GetId() == "SoundEffect");
			CHECK(importer.GetMainType() == AssetType::AudioClip);
			CHECK(importer.GetSettingsTypeName().empty());
			CHECK(importer.CanImport(".sfx"));
			CHECK_FALSE(importer.CanImport(".wav"));
		}

		TEST_CASE("SoundEffectImporter: the example of the architecture reads and writes back canonically" * doctest::skip(true))
		{
			Test::AssetTestFixture fixture;
			SoundEffectLoadReport report;
			const Result<SoundEffectDescription> description = SoundEffectFromText(LineClearText, fixture.GetRegistry(), report, true);
			REQUIRE_MESSAGE(description.has_value(), description.error().ToString());
			CHECK(report.FileVersion == SoundEffectFormatVersion);
			CHECK(report.Diagnostics.empty());
			CHECK(description->Seed == 7);
			REQUIRE(description->Layers.size() == 2);
			CHECK(description->Layers[0].Notes.size() == 4);
			CHECK(description->Layers[1].Wave == SoundWave::Noise);
			const Result<std::string> canonical = SoundEffectToText(*description, fixture.GetRegistry());
			REQUIRE_MESSAGE(canonical.has_value(), canonical.error().ToString());
			// Load, then save, is byte-identical (§6).
			SoundEffectLoadReport again;
			const Result<SoundEffectDescription> reread = SoundEffectFromText(*canonical, fixture.GetRegistry(), again, true);
			REQUIRE(reread.has_value());
			CHECK(*reread == *description);
			const Result<std::string> resaved = SoundEffectToText(*reread, fixture.GetRegistry());
			REQUIRE(resaved.has_value());
			CHECK(*resaved == *canonical);
			CHECK(canonical->starts_with("{\n\t\"Format\": \"SoundEffect\",\n\t\"Version\": 1,"));
		}

		TEST_CASE("SoundEffectImporter: invalid documents are located errors" * doctest::skip(true))
		{
			Test::AssetTestFixture fixture;
			SoundEffectLoadReport report;
			const Result<SoundEffectDescription> badNote = SoundEffectFromText(
				R"({"Format": "SoundEffect", "Version": 1, "Layers": [{"Notes": ["Q4:0.1"]}]})", fixture.GetRegistry(), report);
			REQUIRE_FALSE(badNote.has_value());
			CHECK(badNote.error().GetCode() == ErrorCode::Validation);
			CHECK(badNote.error().ToString().contains("/Layers/0/Notes/0"));
			CHECK(GetErrorCode(SoundEffectFromText(R"({"Format": "Material", "Version": 1})", fixture.GetRegistry(), report)) == ErrorCode::Validation);
			CHECK(GetErrorCode(SoundEffectFromText(R"({"Format": "SoundEffect", "Version": 2, "Layers": []})", fixture.GetRegistry(), report))
				== ErrorCode::UnsupportedVersion);
			CHECK(GetErrorCode(SoundEffectFromText("{", fixture.GetRegistry(), report)) == ErrorCode::Parse);
			CHECK(GetErrorCode(SoundEffectFromText(R"({"Format": "SoundEffect", "Version": 1, "Layers": [{"Wave": "sine", "Duration": 0.1}]})",
					  fixture.GetRegistry(), report))
				== ErrorCode::Validation);

			// Unknown keys warn, or fail when strict.
			SoundEffectLoadReport warnings;
			const std::string_view unknown = R"({"Format": "SoundEffect", "Version": 1, "Layers": [{"Duration": 0.1}], "Reverb": 1})";
			CHECK(SoundEffectFromText(unknown, fixture.GetRegistry(), warnings).has_value());
			CHECK(warnings.Diagnostics.size() == 1);
			CHECK(GetErrorCode(SoundEffectFromText(unknown, fixture.GetRegistry(), warnings, true)) == ErrorCode::Validation);
		}

		TEST_CASE("SoundEffectImporter: a .sfx imports as 48 kHz mono PCM" * doctest::skip(true))
		{
			Test::AssetTestFixture fixture;
			const Result<ImportResult> imported = ImportSoundEffect(fixture, "LineClear.sfx", LineClearText);
			REQUIRE_MESSAGE(imported.has_value(), imported.error().ToString());
			REQUIRE(imported->Artifacts.size() == 1);
			CHECK(imported->Dependencies.empty());
			const Result<AssetRef<AudioClipData>> clip = LoadCookedAudioClip(imported->Artifacts.front().Cooked);
			REQUIRE_MESSAGE(clip.has_value(), clip.error().ToString());
			CHECK((*clip)->Encoding == AudioClipEncoding::Pcm16);
			CHECK((*clip)->SampleRate == 48000);
			CHECK((*clip)->ChannelCount == 1);
			CHECK_FALSE((*clip)->Stream);
			CHECK((*clip)->Bytes.size() == (*clip)->FrameCount * 2);
			// Deterministic: a second import cooks the same bytes.
			const Result<ImportResult> again = ImportSoundEffect(fixture, "LineClear.sfx", LineClearText);
			REQUIRE(again.has_value());
			CHECK(again->Artifacts.front().Cooked == imported->Artifacts.front().Cooked);
		}

		TEST_CASE("SoundEffectImporter: an invalid .sfx is ImportFailed with located issues" * doctest::skip(true))
		{
			Test::AssetTestFixture fixture;
			const Result<ImportResult> rejected = ImportSoundEffect(fixture, "Broken.sfx",
				R"({"Format": "SoundEffect", "Version": 1, "Volume": 3, "Layers": [{"Duration": 0}]})");
			REQUIRE_FALSE(rejected.has_value());
			CHECK(rejected.error().GetCode() == ErrorCode::ImportFailed);
			const std::string message = rejected.error().ToString();
			CHECK(message.contains("Assets/Audio/Broken.sfx"));
			CHECK(message.contains("/Volume"));
			CHECK(message.contains("/Layers/0/Duration"));
		}

		TEST_CASE("SoundEffectImporter: the ten presets import and EngineAssets.json lists them" * doctest::skip(true))
		{
			struct Preset
			{
				std::string_view Name{};
				AssetHandle Handle{};
			};
			constexpr std::array<Preset, 10> Presets = { {
				{ "Click", BuiltinAssetHandles::ClickSound },
				{ "Blip", BuiltinAssetHandles::BlipSound },
				{ "Coin", BuiltinAssetHandles::CoinSound },
				{ "Jump", BuiltinAssetHandles::JumpSound },
				{ "Hit", BuiltinAssetHandles::HitSound },
				{ "Explosion", BuiltinAssetHandles::ExplosionSound },
				{ "PowerUp", BuiltinAssetHandles::PowerUpSound },
				{ "LineClear", BuiltinAssetHandles::LineClearSound },
				{ "Win", BuiltinAssetHandles::WinSound },
				{ "Lose", BuiltinAssetHandles::LoseSound },
			} };
			Result<std::string> catalogText = FileSystem::ReadText(Test::GetRepositoryRoot() / "Resources" / BuiltinAssetCatalog::FileName);
			REQUIRE_MESSAGE(catalogText.has_value(), catalogText.error().ToString());
			const Result<BuiltinAssetCatalog> catalog = BuiltinAssetCatalog::Parse(*catalogText);
			REQUIRE_MESSAGE(catalog.has_value(), catalog.error().ToString());
			Test::AssetTestFixture fixture;
			for (const Preset& preset : Presets)
			{
				CAPTURE(std::string(preset.Name));
				const BuiltinAssetEntry* entry = catalog->Find(preset.Handle);
				REQUIRE(entry != nullptr);
				CHECK(entry->Path == std::string("engine://Audio/") + std::string(preset.Name));
				CHECK(entry->Type == AssetType::AudioClip);
				CHECK(entry->Source == BuiltinAssetSource::File);
				CHECK(entry->File == std::string("Audio/") + std::string(preset.Name) + ".sfx");
				CHECK(entry->Importer == SoundEffectImporter::Id);
				const Result<std::string> text = FileSystem::ReadText(Test::GetRepositoryRoot() / "Resources" / entry->File);
				REQUIRE_MESSAGE(text.has_value(), text.error().ToString());
				const Result<ImportResult> imported = ImportSoundEffect(fixture, std::string(preset.Name) + ".sfx", *text);
				REQUIRE_MESSAGE(imported.has_value(), imported.error().ToString());
				// Every preset file is canonical (§6: load then save is byte-identical).
				SoundEffectLoadReport report;
				const Result<SoundEffectDescription> description = SoundEffectFromText(*text, fixture.GetRegistry(), report, true);
				REQUIRE(description.has_value());
				const Result<std::string> canonical = SoundEffectToText(*description, fixture.GetRegistry());
				REQUIRE(canonical.has_value());
				CHECK(*canonical == *text);
			}
		}
	}

}
