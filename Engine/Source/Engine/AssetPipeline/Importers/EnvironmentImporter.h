#pragma once

#include "Engine/AssetPipeline/IAssetImporter.h"
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
	};

	// Radiance .hdr environments (Architecture §7.4, §8.6): stbi_loadf from memory to RGB32F (an equirectangular image whose
	// width is twice its height; non-finite or negative texels are ImportFailed naming the first one), then the GPU bake of
	// the context's IEnvironmentBaker (Renderer/EnvironmentBaker), cooked as one EnvironmentData (CookEnvironment). Without
	// a baker (--renderer none, tools) the import is Unsupported with the hint "start the editor with a GPU once to bake this
	// environment": a cooked bake under the same cache key (the project's Library/Cache, or the engine cooked cache for the
	// built-ins, §7.5) is used before the importer is ever called, so an environment baked once on a GPU loads everywhere
	// ("test_renderer_none_environment_import_uses_cached_bake"). ".exr" is claimed only to refuse it precisely:
	// ImportFailed "OpenEXR is not supported" with the hint "download the .hdr variant from Poly Haven".
	//
	// Runs on the main thread (RequiresMainThread), because the bake records GPU work (§4.11). Deterministic for a device
	// class: the same source, settings and device give identical cooked bytes. Frozen by the M8 contract
	// (Docs/Decisions/0013-m8-decisions.md decision 9); stream B implements it and registers it (RegisterBuiltinImporters,
	// RegisterAssetPipelineTypes).
	class EnvironmentImporter final : public IAssetImporter
	{
	public:
		static constexpr std::string_view Id = "Environment";
		static constexpr uint32_t Version = 1;

		[[nodiscard]] std::string_view GetId() const override { return Id; }
		[[nodiscard]] uint32_t GetVersion() const override { return Version; }
		[[nodiscard]] AssetType GetMainType() const override { return AssetType::Environment; }
		// ".hdr", ".exr" (refused, see the class comment).
		[[nodiscard]] std::span<const std::string_view> GetExtensions() const override;
		[[nodiscard]] std::string_view GetSettingsTypeName() const override { return "EnvironmentImportSettings"; }
		[[nodiscard]] bool RequiresMainThread() const override { return true; }
		[[nodiscard]] Result<ImportResult> Import(ImportContext& context, const AssetMetadata& metadata) const override;

		// Registers EnvironmentImportSettings.
		static void RegisterTypes(TypeRegistry& registry);
	};

}
