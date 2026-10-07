#pragma once

#include "Engine/AssetPipeline/IAssetImporter.h"
#include "Engine/Core/Base.h"

#include <cstdint>
#include <span>
#include <string_view>

namespace Engine {

	class TypeRegistry;

	// Registry enum "TextureUsage": how a texture's texels are interpreted (§7.4 "sRGB or linear by usage setting").
	enum class TextureUsage : uint8_t
	{
		Color,    // base colour, emissive: TextureFormat::RGBA8Srgb, mips filtered in linear light (sRGB-correct)
		Linear,   // data (metallic-roughness, occlusion, masks): RGBA8Unorm, mips filtered on the stored values
		NormalMap // tangent-space normals: RGBA8Unorm, every generated mip renormalized to unit vectors (level 0 as authored)
	};

	// Registry struct "TextureImportSettings" (§5.4: every *ImportSettings is reflected; a .meta's "Settings").
	struct TextureImportSettings
	{
		TextureUsage Usage = TextureUsage::Color;
		bool GenerateMips = true; // the full chain to 1x1 (§7.4: CPU mips with stb_image_resize2)
	};

	// PNG, JPEG, TGA and BMP (§7.4), decoded from memory with stb_image (through the implementation in
	// Graphics/ThirdParty/StbImageImplementation.cpp, ADR 0009 decision 11), expanded to RGBA8 (grey and grey-alpha
	// replicate, missing alpha becomes 255), mips made with stb_image_resize2 (sRGB-aware for Color; for NormalMap the
	// generated levels are renormalized and level 0 keeps the authored texels), cooked as one TextureData (CookTexture). A
	// width or height above MaxTextureDimension (16384), zero, or a decode failure is ImportFailed naming stb's reason.
	// Deterministic: stb_image_resize2 runs without SIMD-dependent rounding differences across configurations (its
	// implementation TU sets fp_contract off, Vendor/stb/VENDOR.md). The glTF importer reuses ImportTextureFromMemory for
	// its texture sub-assets.
	class TextureImporter final : public IAssetImporter
	{
	public:
		static constexpr std::string_view Id = "Texture";
		static constexpr uint32_t Version = 1;

		[[nodiscard]] std::string_view GetId() const override { return Id; }
		[[nodiscard]] uint32_t GetVersion() const override { return Version; }
		[[nodiscard]] AssetType GetMainType() const override { return AssetType::Texture; }
		// ".png", ".jpg", ".jpeg", ".tga", ".bmp".
		[[nodiscard]] std::span<const std::string_view> GetExtensions() const override;
		[[nodiscard]] std::string_view GetSettingsTypeName() const override { return "TextureImportSettings"; }

		// One artifact: the texture. No sub-assets, no dependencies.
		[[nodiscard]] Result<ImportResult> Import(ImportContext& context, const AssetMetadata& metadata) const override;

		// Decodes an encoded image (PNG, JPEG, TGA or BMP bytes) and builds its cooked texture artifact with `settings`
		// (the texture importer and the glTF importer's texture sub-assets share it). `name` names the image in errors.
		// Errors: ImportFailed as above.
		[[nodiscard]] static Result<Buffer> ImportTextureFromMemory(std::span<const std::byte> encoded, const TextureImportSettings& settings,
			std::string_view name);

		// Registers TextureUsage and TextureImportSettings.
		static void RegisterTypes(TypeRegistry& registry);
	};

}
