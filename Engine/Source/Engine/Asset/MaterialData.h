#pragma once

#include "Engine/Asset/Asset.h"
#include "Engine/Asset/AssetHandle.h"
#include "Engine/Asset/AssetType.h"
#include "Engine/Asset/TypedAssetHandle.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Buffer.h"
#include "Engine/Core/Json/Json.h"
#include "Engine/Core/Json/JsonWriter.h"
#include "Engine/Core/Result.h"
#include "Engine/Reflection/ValidationContext.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Materials (Architecture §6.5, §8.5): the .material file, the reflected MaterialData struct behind it (§5.4: "StructInfo
// (... MaterialData ...)"), its cooked payload (§6.8: minified canonical JSON) and the loaded asset. Produced by
// MaterialImporter (.material files), GltfImporter (material sub-assets) and the built-in Default and Error materials.

namespace Engine {

	class TypeRegistry;

	// Registry enum "AlphaMode" (glTF 2.0 alpha modes, §8.5).
	enum class AlphaMode : uint8_t
	{
		Opaque,
		Mask,
		Blend
	};

	// A PBR metallic-roughness material (§6.5, §8.5), registry struct "Material". Every field is serialized under its member
	// name, in member order, after "Format": "Material" and "Version": 1. Texture slots are null for "no texture" (the
	// renderer binds White, FlatNormal or Black then, §8.4). Defaults and bounds (also enforced on every write path, §5.4):
	//   BaseColor (1, 1, 1, 1), each 0 to 1 (linear; the map is sRGB)   Metallic 0, Roughness 0.5, both 0 to 1
	//   NormalScale 1, >= 0          OcclusionStrength 1, 0 to 1        Emissive (0, 0, 0), each 0 to 1
	//   EmissiveStrength 1, >= 0     AlphaMode Opaque                   AlphaCutoff 0.5, 0 to 1
	//   DoubleSided false            UVScale (1, 1), UVOffset (0, 0), finite
	// Colours are limited to 0..1 like every colour field (ADR 0006); HDR emission comes from EmissiveStrength. Plain data;
	// immutable once loaded (AssetRef<MaterialData>).
	struct MaterialData : Asset
	{
		static constexpr AssetType StaticType = AssetType::Material;
		static constexpr std::string_view FormatName = "Material";
		static constexpr uint32_t CurrentVersion = 1;
		// The cooked payload layout (CookedHeader::FormatVersion): the minified canonical document.
		static constexpr uint16_t FormatVersion = 1;

		MaterialData()
			: Asset(StaticType)
		{
		}

		glm::vec4 BaseColor{ 1.0f, 1.0f, 1.0f, 1.0f };
		TypedAssetHandle<AssetType::Texture> BaseColorMap{};
		float Metallic = 0.0f;
		float Roughness = 0.5f;
		TypedAssetHandle<AssetType::Texture> MetallicRoughnessMap{}; // G = roughness, B = metallic (glTF)
		TypedAssetHandle<AssetType::Texture> NormalMap{};            // tangent space
		float NormalScale = 1.0f;
		TypedAssetHandle<AssetType::Texture> OcclusionMap{}; // R
		float OcclusionStrength = 1.0f;
		glm::vec3 Emissive{ 0.0f, 0.0f, 0.0f };
		float EmissiveStrength = 1.0f;
		TypedAssetHandle<AssetType::Texture> EmissiveMap{};
		Engine::AlphaMode AlphaMode = Engine::AlphaMode::Opaque; // the type is spelled Engine:: (GCC's "changes meaning", ADR 0006)
		float AlphaCutoff = 0.5f;
		bool DoubleSided = false;
		glm::vec2 UVScale{ 1.0f, 1.0f };
		glm::vec2 UVOffset{ 0.0f, 0.0f };
	};

	// Registers the enum "AlphaMode" and the struct "Material" (MaterialData; colours as ColorFields, the bounds above, a
	// Generate hook when a Validate rule exists, §5.4).
	void RegisterMaterialTypes(TypeRegistry& registry);

	// The texture handles `material` references, without nulls, sorted and unique: its dependencies in the
	// AssetDependencyGraph (§7.5 "material -> textures").
	[[nodiscard]] std::vector<AssetHandle> GetMaterialTextureHandles(const MaterialData& material);

	// What a material load reports besides its result: the file's version and its warnings (unknown members) in document
	// order.
	struct MaterialLoadReport
	{
		uint32_t FileVersion = 0;
		std::vector<ValidationIssue> Diagnostics{};
	};

	// The canonical .material document ("Format", "Version", then the registry fields). Errors: Validation for a value that
	// cannot be written (non-finite), located. Requires a frozen registry on which RegisterMaterialTypes ran (asserted).
	[[nodiscard]] Result<Json> MaterialToJson(const MaterialData& material, const TypeRegistry& registry);

	// The canonical text of MaterialToJson: Pretty for .material files, Minified for the cooked payload.
	[[nodiscard]] Result<std::string> MaterialToText(const MaterialData& material, const TypeRegistry& registry,
		JsonStyle style = JsonStyle::Pretty);

	// Reads a .material document strictly through the registry (StructInfo::FromJson of "Material": missing keys keep their
	// defaults, unknown keys warn into `report`, or fail with `strictUnknowns`, every value validated, enum names exact).
	// Errors: Validation (located, with every bad field as an issue) for a wrong "Format", a version below 1, a wrong JSON
	// type or an out-of-range value; UnsupportedVersion naming both versions for a newer file.
	[[nodiscard]] Result<MaterialData> MaterialFromJson(const Json& document, const TypeRegistry& registry, MaterialLoadReport& report,
		bool strictUnknowns = false);

	// JsonReader::Parse, then MaterialFromJson. Errors: Parse (line and column), and as MaterialFromJson.
	[[nodiscard]] Result<MaterialData> MaterialFromText(std::string_view text, const TypeRegistry& registry, MaterialLoadReport& report,
		bool strictUnknowns = false);

	// The complete cooked artifact: CookedHeader + the minified canonical document (§6.8). Errors: as MaterialToText.
	[[nodiscard]] Result<Buffer> CookMaterial(const MaterialData& material, const TypeRegistry& registry, uint32_t importerVersion);

	// The material of a cooked artifact, read strictly (unknown members are errors: cooked data is engine-written). Errors:
	// as ReadCookedArtifact and MaterialFromText.
	[[nodiscard]] Result<AssetRef<MaterialData>> LoadCookedMaterial(std::span<const std::byte> cooked, const TypeRegistry& registry);

}
