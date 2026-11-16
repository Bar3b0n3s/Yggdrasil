#pragma once

#include "Engine/AssetPipeline/IAssetImporter.h"
#include "Engine/AssetPipeline/Importers/TextureImporter.h"
#include "Engine/Core/Base.h"

#include <cstdint>
#include <span>
#include <string_view>

namespace Engine {

	class TypeRegistry;

	// Registry struct "EnvironmentImportSettings" (§8.6 step 1).
	struct EnvironmentImportSettings
	{
		// Scales texels whose Rec. 709 luminance exceeds MaxLuminance down to it before baking (default off: filtered
		// importance sampling handles bright suns, §8.6 step 3; the sun-heavy built-in Sky turns it on in EngineAssets.json).
		bool ClampLuminance = false;
		float MaxLuminance = 1000.0f; // 1 to 65504 (the largest binary16 value)
		// M23 (C23.1, R-TEXC-HDR; Docs/Decisions/0034-m23-contract.md decision 14; EnvironmentImporter version 2): Auto
		// stores the skybox and specular cubes as BC6H (CubeMapFormat::BC6HUfloat), None as RGBA16F. Appended after
		// MaxLuminance, so earlier .meta settings, which do not hold it, and the built-in environments, whose
		// EngineAssets.json settings do not name it, read as Auto. The SH9 irradiance is the same either way.
		TextureCompression Compression = TextureCompression::Auto;
	};

	// Equirectangular environments (Architecture §7.4, §8.6), decoded to RGB32F, then the GPU bake of the context's
	// IEnvironmentBaker (Renderer/EnvironmentBaker), cooked as one EnvironmentData (CookEnvironment):
	//   .hdr  Radiance RGBE, stbi_loadf from memory.
	//   .exr  OpenEXR through tinyexr (Vendor/tinyexr; M23, A-EXR, Docs/Decisions/0034-m23-contract.md decision 14):
	//         single-part scanline and tiled images (of one level, mipmapped or ripmapped tiles: the top level is read)
	//         compressed with NONE, RLE, ZIPS, ZIP, PIZ or PXR24, their R, G and B channels (half, float or uint, uint
	//         converted to the nearest float) in the data window. Multi-part and deep images, the B44, B44A, DWAA and DWAB
	//         compressions, a missing R, G or B channel, a subsampled channel, and an image whose decoded channels would
	//         exceed MaxOpenExrDecodeBytes are ImportFailed naming what is unsupported, before any pixel is decoded. So is a file
	//         whose offset table names a chunk other than the one at its place (a scanline block that does not start at its
	//         line, a tile at other coordinates or another level), which tinyexr would decode into memory it never wrote.
	// Both: an equirectangular image twice as wide as high, at most 8,192 texels wide (checked from the header before any
	// texel is decoded); a non-finite or negative texel is ImportFailed naming the first one. Compression Auto then stores
	// both baked cubes as BC6H blocks (CompressTextureBlocks, every face of every level), None keeps them RGBA16F.
	// Without a baker (--renderer none, tools) the import is Unsupported with the hint "start the editor with a GPU once to
	// bake this environment", once the source has been read and decoded (a source that cannot be is ImportFailed, as with
	// a baker): a cooked bake under the same cache key (the project's Library/Cache, or the engine cooked cache for the
	// built-ins, §7.5) is used before the importer is ever called, so an environment baked once on a GPU loads everywhere
	// ("test_renderer_none_environment_import_uses_cached_bake"). Unsupported means that and nothing else: the
	// EditorAssetManager defers such a bake to the next editor with a GPU, a warning rather than an asset error (World Tree
	// brief 14, EditorAssetManager.h).
	//
	// Runs on the main thread (RequiresMainThread), because the bake records GPU work (§4.11). Deterministic for a device
	// class: the same source, settings and device give identical cooked bytes (the decoders read every texel they return
	// from the source, and the BC6H codec compresses alike in every configuration, Asset/TextureCompression.h). Frozen by the
	// M8 contract (Docs/Decisions/0013-m8-decisions.md decision 9); version 2 by M23 (OpenEXR and the Compression setting).
	// Registered by RegisterBuiltinImporters, its settings by RegisterAssetPipelineTypes.
	class EnvironmentImporter final : public IAssetImporter
	{
	public:
		static constexpr std::string_view Id = "Environment";
		static constexpr uint32_t Version = 2;
		// The hint of an environment no editor here can bake (no baker, no cooked bake under its cache key, §7.4, §13.9): the
		// import's, and the exporter's for a built-in environment Engine.pak leaves out (EditorCore/Export/Exporter.h).
		static constexpr std::string_view GpuHint = "start the editor with a GPU once to bake this environment";
		// The most bytes an OpenEXR image's decoded channels may take (every channel of the file, R, G and B as 32-bit
		// floats; tinyexr decodes them all): 1 GiB, eight float channels of an 8,192 x 4,096 image.
		static constexpr uint64_t MaxOpenExrDecodeBytes = uint64_t{ 1 } << 30;

		[[nodiscard]] std::string_view GetId() const override { return Id; }
		[[nodiscard]] uint32_t GetVersion() const override { return Version; }
		[[nodiscard]] AssetType GetMainType() const override { return AssetType::Environment; }
		// ".hdr", ".exr".
		[[nodiscard]] std::span<const std::string_view> GetExtensions() const override;
		[[nodiscard]] std::string_view GetSettingsTypeName() const override { return "EnvironmentImportSettings"; }
		[[nodiscard]] bool RequiresMainThread() const override { return true; }
		[[nodiscard]] Result<ImportResult> Import(ImportContext& context, const AssetMetadata& metadata) const override;

		// Registers EnvironmentImportSettings (after TextureImporter::RegisterTypes, which registers TextureCompression).
		static void RegisterTypes(TypeRegistry& registry);
	};

}
