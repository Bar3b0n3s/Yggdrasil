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

	// M23 (C23.1, Material version 2; Docs/Decisions/0034-m23-contract.md decision 10): registry enum "TextureCoordinateSet",
	// the texture coordinates a map samples (glTF textureInfo.texCoord 0 or 1): UV0, the mesh's TexCoord (MeshVertex), or
	// UV1, its second set (MeshVertexAttributes::TexCoord1; (0, 0) on a mesh without the attribute stream).
	enum class TextureCoordinateSet : uint8_t
	{
		UV0,
		UV1
	};

	// M46 (Material version 3; Docs/Decisions/0064-m46-mesh-lods-draw-distance-and-wind.md decision 3H): registry struct
	// "MaterialWind", how a material's surface answers the scene's Wind (Scene/Components/WindComponent.h). The renderer
	// displaces the positions of a mesh drawn with an Enabled block alike in every pipeline (Common/Wind.slang): they lean
	// with the wind (its gusted speed along it and its turbulent part across it) by Bend times their squared height above
	// the entity's origin, sway along it by Branch times the mesh's _WIND x weight and flutter along their normals by Flutter
	// times its z weight, each scaled by the response x (2 - |x|), x = speed * (1 - Stiffness) / 10 m/s clamped to [-1, 1]
	// (0 in a calm wind; a Stiffness of 0 saturates at 10 m/s, 1 never moves). The flutter moves along the normal through
	// the entity's transform, so Flutter is in metres at the entity's scale 1 and scales with the entity; the lean and the
	// sway are in world metres. Defaults and bounds:
	//   Enabled false     Bend 0.1, 0 to 1 (about Bend m of lean at 1 m of height in a 10 m/s wind)
	//   Branch 0.1 m, 0 to 1          Flutter 0.02 m, 0 to 0.25          Stiffness 0.5, 0 to 1
	struct MaterialWind
	{
		bool Enabled = false;
		float Bend = 0.1f;
		float Branch = 0.1f;
		float Flutter = 0.02f;
		float Stiffness = 0.5f;

		bool operator==(const MaterialWind&) const = default;
	};

	// A PBR metallic-roughness material (§6.5, §8.5), registry struct "Material". Every field is serialized under its member
	// name, in member order, after "Format": "Material" and "Version": 3. Texture slots are null for "no texture" (the
	// renderer binds White, FlatNormal or Black then, §8.4); each map's UV set follows it (M23, version 2: UV0 by default, and
	// a version 1 file, which has none, loads with UV0 everywhere); the Wind block follows UVOffset (M46, version 3: a version
	// 1 or 2 file, which has none, loads with wind disabled). Defaults and bounds (also enforced on every write path, §5.4):
	//   BaseColor (1, 1, 1, 1), each 0 to 1 (linear; the map is sRGB)   Metallic 0, Roughness 0.5, both 0 to 1
	//   NormalScale 1, >= 0          OcclusionStrength 1, 0 to 1        Emissive (0, 0, 0), each 0 to 1
	//   EmissiveStrength 1, >= 0     AlphaMode Opaque                   AlphaCutoff 0.5, 0 to 1
	//   DoubleSided false            UVScale (1, 1), UVOffset (0, 0), finite
	//   Wind: MaterialWind's defaults (disabled)
	// Colours are limited to 0..1 like every colour field (ADR 0006); HDR emission comes from EmissiveStrength. Plain data;
	// immutable once loaded (AssetRef<MaterialData>).
	struct MaterialData : Asset
	{
		static constexpr AssetType StaticType = AssetType::Material;
		static constexpr std::string_view FormatName = "Material";
		// M23 (Material version 2; Docs/Decisions/0034-m23-contract.md decision 10): the five <Map>UVSet fields. Version 1
		// files load with UV0 sets. M46 (version 3; Docs/Decisions/0064-m46-mesh-lods-draw-distance-and-wind.md decision 3H):
		// the Wind block. Versions 1 and 2 load with wind disabled and save as version 3.
		static constexpr uint32_t CurrentVersion = 3;
		// The cooked payload layout (CookedHeader::FormatVersion): the minified canonical document, version 3 with the Wind
		// block. A cached artifact of another version is rebuilt (§7.5).
		static constexpr uint16_t FormatVersion = 3;

		MaterialData()
			: Asset(StaticType)
		{
		}

		glm::vec4 BaseColor{ 1.0f, 1.0f, 1.0f, 1.0f };
		TypedAssetHandle<AssetType::Texture> BaseColorMap{};
		// M23 (Material version 2, decision 10): each map's texture coordinate set follows its map (MaterialConstants'
		// MaterialFlag<Map>UV1 bits); one UVScale and UVOffset still transform every map.
		Engine::TextureCoordinateSet BaseColorUVSet = Engine::TextureCoordinateSet::UV0;
		float Metallic = 0.0f;
		float Roughness = 0.5f;
		TypedAssetHandle<AssetType::Texture> MetallicRoughnessMap{}; // G = roughness, B = metallic (glTF)
		Engine::TextureCoordinateSet MetallicRoughnessUVSet = Engine::TextureCoordinateSet::UV0;
		TypedAssetHandle<AssetType::Texture> NormalMap{}; // tangent space
		Engine::TextureCoordinateSet NormalUVSet = Engine::TextureCoordinateSet::UV0;
		float NormalScale = 1.0f;
		TypedAssetHandle<AssetType::Texture> OcclusionMap{}; // R
		Engine::TextureCoordinateSet OcclusionUVSet = Engine::TextureCoordinateSet::UV0;
		float OcclusionStrength = 1.0f;
		glm::vec3 Emissive{ 0.0f, 0.0f, 0.0f };
		float EmissiveStrength = 1.0f;
		TypedAssetHandle<AssetType::Texture> EmissiveMap{};
		Engine::TextureCoordinateSet EmissiveUVSet = Engine::TextureCoordinateSet::UV0;
		Engine::AlphaMode AlphaMode = Engine::AlphaMode::Opaque; // the type is spelled Engine:: (GCC's "changes meaning", ADR 0006)
		float AlphaCutoff = 0.5f;
		bool DoubleSided = false;
		glm::vec2 UVScale{ 1.0f, 1.0f };
		glm::vec2 UVOffset{ 0.0f, 0.0f };
		// M46 (Material version 3): how the surface moves in the scene's wind; disabled by default.
		MaterialWind Wind{};
	};

	// M46: registers the struct "MaterialWind" unless `registry` has it already, so the Material struct and the glTF import
	// settings (GltfImporter::RegisterTypes, whose Wind setting is one) may register in either order.
	void RegisterMaterialWindType(TypeRegistry& registry);

	// Registers the enums "AlphaMode" and "TextureCoordinateSet" and the structs "MaterialWind" (RegisterMaterialWindType) and
	// "Material" (MaterialData; colours as ColorFields, the bounds above, a Generate hook when a Validate rule exists, §5.4).
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
	// Versions 1 to 3 read alike: version 1 has no UV set keys, which keep their default UV0, and versions 1 and 2 have no
	// Wind key, which keeps its default (disabled). Errors: Validation (located,
	// with every bad field as an issue) for a wrong "Format", a version below 1, a wrong JSON type or an out-of-range value;
	// UnsupportedVersion naming both versions for a newer file.
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
