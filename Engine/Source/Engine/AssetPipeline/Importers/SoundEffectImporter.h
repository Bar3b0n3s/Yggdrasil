#pragma once

#include "Engine/AssetPipeline/IAssetImporter.h"
#include "Engine/Audio/SoundSynth.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Json/Json.h"
#include "Engine/Core/Json/JsonWriter.h"
#include "Engine/Core/Result.h"
#include "Engine/Reflection/ValidationContext.h"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The .sfx sound effect (Architecture §6.6, §10.3): its document functions, which asset.create, asset.getProperties and
// asset.setProperties use like the material ones (Asset/MaterialData.h), and its importer, which synthesizes it
// (Audio/SoundSynth) into a Pcm16 AudioClipData. The description struct lives in the Audio module, which includes only
// Core, so its reflected types are registered here (RegisterTypes). Frozen by the M12 contract
// (Docs/Decisions/0015-m12-decisions.md).

namespace Engine {

	class TypeRegistry;

	// The .sfx document header (§6.6).
	inline constexpr std::string_view SoundEffectFormatName = "SoundEffect";
	inline constexpr uint32_t SoundEffectFormatVersion = 1;

	// What reading a .sfx document reported besides errors.
	struct SoundEffectLoadReport
	{
		uint32_t FileVersion = 0;
		std::vector<ValidationIssue> Diagnostics{}; // unknown keys (warnings), located
	};

	// The canonical .sfx document: "Format": "SoundEffect", "Version": 1, then the registry fields of "SoundEffect" in
	// member order (Seed, Volume, Layers; every field written, §6). Errors: Validation for a value that cannot be written
	// (non-finite), located. Requires a frozen registry on which SoundEffectImporter::RegisterTypes ran (asserted).
	[[nodiscard]] Result<Json> SoundEffectToJson(const SoundEffectDescription& description, const TypeRegistry& registry);

	// The canonical text of SoundEffectToJson: Pretty for .sfx files.
	[[nodiscard]] Result<std::string> SoundEffectToText(const SoundEffectDescription& description, const TypeRegistry& registry,
		JsonStyle style = JsonStyle::Pretty);

	// Reads a .sfx document strictly through the registry (StructInfo::FromJson of "SoundEffect": missing keys keep their
	// defaults, unknown keys warn into `report` or fail with `strictUnknowns`, enum names exact), then ValidateSoundEffect.
	// Errors: Validation (located, every bad field an issue) for a wrong "Format", a version below 1, a wrong JSON type, an
	// out-of-range value or a ValidateSoundEffect violation; UnsupportedVersion naming both versions for a newer file.
	[[nodiscard]] Result<SoundEffectDescription> SoundEffectFromJson(const Json& document, const TypeRegistry& registry,
		SoundEffectLoadReport& report, bool strictUnknowns = false);

	// JsonReader::Parse, then SoundEffectFromJson. Errors: Parse (line and column), and as SoundEffectFromJson.
	[[nodiscard]] Result<SoundEffectDescription> SoundEffectFromText(std::string_view text, const TypeRegistry& registry,
		SoundEffectLoadReport& report, bool strictUnknowns = false);

	// .sfx (Architecture §7.4 SoundEffectImporter): SoundEffectFromText, SynthesizeSoundEffect, then one AudioClipData of
	// Encoding Pcm16, 48 kHz, mono, Stream false (CookAudioClip). The importer has no settings. A .sfx that does not read or
	// validate is ImportFailed with the located issues. Pure function of (bytes, version); identical bytes in every run,
	// configuration and platform (SoundSynth's determinism). The ten built-in presets engine://Audio/{Click, Blip, Coin, Jump,
	// Hit, Explosion, PowerUp, LineClear, Win, Lose} (Resources/Audio/*.sfx, §7.1) go through it into the engine cooked cache.
	class SoundEffectImporter final : public IAssetImporter
	{
	public:
		static constexpr std::string_view Id = "SoundEffect";
		static constexpr uint32_t Version = 1;

		[[nodiscard]] std::string_view GetId() const override { return Id; }
		[[nodiscard]] uint32_t GetVersion() const override { return Version; }
		[[nodiscard]] AssetType GetMainType() const override { return AssetType::AudioClip; }
		// ".sfx".
		[[nodiscard]] std::span<const std::string_view> GetExtensions() const override;
		[[nodiscard]] std::string_view GetSettingsTypeName() const override { return {}; }

		[[nodiscard]] Result<ImportResult> Import(ImportContext& context, const AssetMetadata& metadata) const override;

		// Registers the enum "SoundWave" and the structs "SoundEnvelope", "SoundLayer" and "SoundEffect" with the ranges of
		// Audio/SoundSynth.h and the description's cross-field rules (ValidateSoundEffect) as Validate hooks
		// (RegisterAssetPipelineTypes calls it): "SoundLayer"'s hook checks one layer's rules, so a bad layer is reported
		// even when another field of the sound is invalid, and "SoundEffect"'s runs ValidateSoundEffect except on an empty
		// Layers list, the registry's default object (SoundEffectFromJson and asset.create still require a layer through
		// ValidateSoundEffect). Both have Generate hooks that keep the registry's random round trips valid.
		static void RegisterTypes(TypeRegistry& registry);
	};

}
