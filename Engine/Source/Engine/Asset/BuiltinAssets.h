#pragma once

#include "Engine/Asset/Asset.h"
#include "Engine/Asset/AssetHandle.h"
#include "Engine/Asset/AssetType.h"
#include "Engine/Asset/MaterialData.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Reflection/VariantValue.h"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Built-in assets (Architecture §4.8 reserved handles, §7.1): procedural meshes, textures and materials generated in code,
// shipped resources (the Default font, the default environments) cooked from Resources/ into the engine cooked cache
// (bin/EngineCache, §7.5) and Engine.pak (§7.6), and generated resources that code above Asset produces at bake time (the
// blue-noise texture, M8, §8.4). Resources/EngineAssets.json names every built-in handle; the constants
// below mirror it, and "BuiltinAssets: EngineAssets.json names exactly the compiled-in handles" keeps both in step.

namespace Engine {

	class VirtualFileSystem;

	// The reserved built-in range (§4.8): 0x0000000000000001 to 0x00000000000003ff. Never generated for project assets.
	inline constexpr uint64_t FirstBuiltinAssetHandle = 0x001;
	inline constexpr uint64_t LastBuiltinAssetHandle = 0x3ff;

	[[nodiscard]] constexpr bool IsBuiltinAssetHandle(AssetHandle handle)
	{
		return handle.GetValue() >= FirstBuiltinAssetHandle && handle.GetValue() <= LastBuiltinAssetHandle;
	}

	// The built-in handles, grouped in blocks of 0x40 per type. The audio block (M12, Docs/Decisions/0015-m12-decisions.md)
	// holds the ten sound-effect presets and the silent clip.
	struct BuiltinAssetHandles
	{
		BuiltinAssetHandles() = delete;

		// engine://Meshes/{Cube, Sphere, Plane, Quad, Cylinder, Capsule, Cone} (procedural, unit size, §7.1).
		static constexpr AssetHandle CubeMesh{ 0x0101 };
		static constexpr AssetHandle SphereMesh{ 0x0102 };
		static constexpr AssetHandle PlaneMesh{ 0x0103 };
		static constexpr AssetHandle QuadMesh{ 0x0104 };
		static constexpr AssetHandle CylinderMesh{ 0x0105 };
		static constexpr AssetHandle CapsuleMesh{ 0x0106 };
		static constexpr AssetHandle ConeMesh{ 0x0107 };
		// engine://Materials/{Default, Error} (procedural).
		static constexpr AssetHandle DefaultMaterial{ 0x0141 };
		static constexpr AssetHandle ErrorMaterial{ 0x0142 };
		// engine://Textures/{White, Black, FlatNormal, Checker, Missing} (procedural).
		static constexpr AssetHandle WhiteTexture{ 0x0181 };
		static constexpr AssetHandle BlackTexture{ 0x0182 };
		static constexpr AssetHandle FlatNormalTexture{ 0x0183 };
		static constexpr AssetHandle CheckerTexture{ 0x0184 };
		static constexpr AssetHandle MissingTexture{ 0x0185 };
		// engine://Textures/BlueNoise (M8, §8.4): the tonemap's 64x64 R8 dither texture, a Generated entry whose generator
		// "BlueNoise" (Renderer/BlueNoise.h) the editor registers (EditorCore/EngineAssetGenerators.h;
		// Docs/Decisions/0013-m8-decisions.md decision 8).
		static constexpr AssetHandle BlueNoiseTexture{ 0x0186 };
		// engine://Fonts/Default: Inter Regular (SIL OFL), Resources/Fonts/Inter-Regular.ttf through FontImporter.
		static constexpr AssetHandle DefaultFont{ 0x01c1 };
		// engine://Environments/{Studio, Sky}: Poly Haven CC0 studio_small_09 and kloofendal_48d_partly_cloudy_puresky at 1k,
		// Resources/Environments/*.hdr through EnvironmentImporter (M8), baked with a GPU into the engine cooked cache
		// (Editor --bake-engine-assets) and shipped in Engine.pak.
		static constexpr AssetHandle StudioEnvironment{ 0x0201 };
		static constexpr AssetHandle SkyEnvironment{ 0x0202 };
		// M12: engine://Audio/{Click, Blip, Coin, Jump, Hit, Explosion, PowerUp, LineClear, Win, Lose}, the sound-effect presets
		// (§7.1, §10.3): File entries, Resources/Audio/<Name>.sfx through SoundEffectImporter.
		static constexpr AssetHandle ClickSound{ 0x0241 };
		static constexpr AssetHandle BlipSound{ 0x0242 };
		static constexpr AssetHandle CoinSound{ 0x0243 };
		static constexpr AssetHandle JumpSound{ 0x0244 };
		static constexpr AssetHandle HitSound{ 0x0245 };
		static constexpr AssetHandle ExplosionSound{ 0x0246 };
		static constexpr AssetHandle PowerUpSound{ 0x0247 };
		static constexpr AssetHandle LineClearSound{ 0x0248 };
		static constexpr AssetHandle WinSound{ 0x0249 };
		static constexpr AssetHandle LoseSound{ 0x024a };
		// M12: engine://Audio/Silence (procedural): 0.1 s of 48 kHz mono silence (CreateSilentAudioClip), the AudioClip
		// placeholder (§7.2 "silent clip").
		static constexpr AssetHandle SilentClip{ 0x024b };
	};

	// The placeholder of a type (§7.2 failure policy): the unit Cube mesh, the Missing checker texture, the Error material,
	// the Default font and the silent clip (M12); null for the other types (scenes, prefabs, scripts and replays report their
	// failure to the caller; an environment has no placeholder asset: the renderer lights with the EnvironmentComponent's
	// FallbackColor instead, GpuResourceCache::GetEnvironment).
	[[nodiscard]] constexpr AssetHandle GetPlaceholderHandle(AssetType type)
	{
		switch (type)
		{
			case AssetType::Mesh:      return BuiltinAssetHandles::CubeMesh;
			case AssetType::Texture:   return BuiltinAssetHandles::MissingTexture;
			case AssetType::Material:  return BuiltinAssetHandles::ErrorMaterial;
			case AssetType::Font:      return BuiltinAssetHandles::DefaultFont;
			case AssetType::AudioClip: return BuiltinAssetHandles::SilentClip;
			default:                   return AssetHandle();
		}
	}

	// Where a built-in comes from.
	enum class BuiltinAssetSource : uint8_t
	{
		// Generated by the Asset module in code (CreateProceduralBuiltinAsset); never cached; cannot fail.
		Procedural,
		// A file under Resources/ (engine://), cooked by an importer with the entry's settings into the engine cooked cache.
		File,
		// Generated in code by a module above Asset (the blue-noise texture: Renderer/BlueNoise.cpp, M8, §8.4) through an
		// EngineAssetGenerator (AssetPipeline/EngineAssetBaker.h), cooked into the engine cooked cache.
		Generated
	};

	// One built-in asset, one entry of Resources/EngineAssets.json.
	struct BuiltinAssetEntry
	{
		AssetHandle Handle{}; // in the reserved range
		std::string Path{};   // its engine path, "engine://Meshes/Cube" (an AssetReference of kind EnginePath)
		AssetType Type = AssetType::None;
		BuiltinAssetSource Source = BuiltinAssetSource::Procedural;
		std::string File{};     // Source File: the file below engine://, "Fonts/Inter-Regular.ttf"; empty otherwise
		std::string Importer{}; // Source File: the importer id, "Font"; empty otherwise
		// Source File: the import settings that differ from the importer's defaults, a JSON object merged over them like
		// asset.setImportSettings (§8.6's ClampLuminance for the sun-heavy Sky HDRI). JSON null when nothing differs, and
		// always for Procedural and Generated entries: the file then writes {}, which Parse reads back as null, so the
		// in-memory form is canonical. The cache key uses the complete merged settings (EngineAssetBaker.h).
		VariantValue Settings{};
		std::string Generator{}; // Source Generated: the generator id, "BlueNoise"; empty otherwise

		bool operator==(const BuiltinAssetEntry&) const = default;
	};

	// Resources/EngineAssets.json, canonical JSON (JsonWriter Pretty):
	//     { "Format": "EngineAssets", "Version": 1,
	//       "Assets": [ { "Handle": "0000000000000101", "Path": "engine://Meshes/Cube", "Type": "Mesh", "Source": "Procedural",
	//                     "File": "", "Importer": "", "Settings": {}, "Generator": "" }, ... ] }
	// "Source" is "Procedural", "File" or "Generated". Entries sorted by handle; every member written. Immutable after
	// loading; thread-safe to read.
	class BuiltinAssetCatalog
	{
	public:
		static constexpr std::string_view FormatName = "EngineAssets";
		static constexpr uint32_t CurrentVersion = 1;
		// The catalogue's path below engine://.
		static constexpr std::string_view FileName = "EngineAssets.json";

		BuiltinAssetCatalog() = default;

		// Parses the catalogue. Errors: Parse; Validation (located) for a wrong format, a handle outside the reserved range or
		// repeated, unsorted entries, a path that is not an engine path or is repeated, a None or unknown type, an unknown
		// Source, Settings that are not an object ({} is read as null), a File entry without a file or importer or with a
		// generator, a Generated
		// entry without a generator or with a file, an importer or non-empty settings, a Procedural entry with any of them,
		// or a Procedural entry the code cannot generate; UnsupportedVersion for a newer file. (File settings are validated
		// against the importer's settings struct when the entry is baked.)
		[[nodiscard]] static Result<BuiltinAssetCatalog> Parse(std::string_view text);

		// Reads engine://EngineAssets.json and parses it. Errors: those of VirtualFileSystem::ReadText and Parse.
		[[nodiscard]] static Result<BuiltinAssetCatalog> Load(const VirtualFileSystem& vfs);

		// The canonical text (the layout above).
		[[nodiscard]] std::string ToText() const;

		[[nodiscard]] std::span<const BuiltinAssetEntry> GetEntries() const { return m_Entries; }
		[[nodiscard]] const BuiltinAssetEntry* Find(AssetHandle handle) const;
		// The entry whose Path is exactly `path` ("engine://Meshes/Cube"), or nullptr.
		[[nodiscard]] const BuiltinAssetEntry* FindByPath(std::string_view path) const;
	private:
		std::vector<BuiltinAssetEntry> m_Entries; // sorted by handle
	};

	// The catalogue entries of the procedural built-ins, compiled in (the meshes, materials and textures above, and the silent
	// clip, M12), sorted by handle. Both asset managers serve these without reading anything, so placeholders never fail.
	[[nodiscard]] std::span<const BuiltinAssetEntry> GetProceduralBuiltinEntries();

	// The procedural built-in `handle`, generated deterministically (identical bytes on every run and configuration).
	// Errors: NotFound when `handle` is not a procedural built-in.
	[[nodiscard]] Result<AssetRef<Asset>> CreateProceduralBuiltinAsset(AssetHandle handle);

	// The built-in materials: Default (white, roughness 0.5, no maps) and Error (magenta, unlit-looking: emissive magenta,
	// so a missing material is obvious under any lighting).
	enum class BuiltinMaterial : uint8_t
	{
		Default,
		Error
	};

	[[nodiscard]] MaterialData CreateBuiltinMaterial(BuiltinMaterial material);

}
