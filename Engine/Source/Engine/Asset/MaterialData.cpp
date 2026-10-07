#include "EnginePCH.h"
#include "Engine/Asset/MaterialData.h"

#include "Engine/Asset/CookedFormat.h"
#include "Engine/Core/Assert.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Reflection/TypeRegistry.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <utility>

namespace Engine {

	namespace Utils {

		// The oldest readable .material version (there has been no other yet).
		static constexpr uint32_t MinimumMaterialVersion = 1;

		// A linear colour channel or a factor in [0, 1].
		static FieldMeta UnitRange()
		{
			FieldMeta meta;
			meta.Min = 0.0;
			meta.Max = 1.0;
			return meta;
		}

		// A strength or scale that may exceed 1 but not go negative.
		static FieldMeta NonNegative()
		{
			FieldMeta meta;
			meta.Min = 0.0;
			return meta;
		}

		// The registered "Material" struct. A registry without it is a programmer error (asserted); the error return keeps
		// builds without asserts safe.
		[[nodiscard]] static Result<const StructInfo*> FindMaterialType(const TypeRegistry& registry)
		{
			ENGINE_CORE_ASSERT(registry.IsFrozen(), "Materials need a frozen type registry");
			const StructInfo* type = registry.FindStruct<MaterialData>();
			ENGINE_CORE_ASSERT(type != nullptr, "Materials need a registry on which RegisterMaterialTypes ran");
			if (type == nullptr)
				return MakeError(ErrorCode::InvalidState, "the type registry has no Material struct (RegisterMaterialTypes was not called)");
			return type;
		}

		[[nodiscard]] static Result<MaterialData> ReadMaterialDocument(const Json& document, const TypeRegistry& registry,
			MaterialLoadReport& report, bool strictUnknowns)
		{
			ENGINE_TRY_ASSIGN(const StructInfo* type, FindMaterialType(registry));
			const JsonReader root(document);
			ENGINE_TRY_ASSIGN(report.FileVersion,
				root.ReadFormatHeader(MaterialData::FormatName, MinimumMaterialVersion, MaterialData::CurrentVersion));

			// Everything below the header is the Material object; the header check above guarantees an object.
			Json fields = document;
			fields.erase("Format");
			fields.erase("Version");

			MaterialData material;
			ReadContext context;
			context.Strict = strictUnknowns;
			context.Diagnostics = &report.Diagnostics;
			ENGINE_TRY(type->FromJson(&material, JsonReader(fields), context));
			return material;
		}

	}

	void RegisterMaterialTypes(TypeRegistry& registry)
	{
		registry.Enum<AlphaMode>("AlphaMode", "How a material's base colour alpha is used (the glTF 2.0 alpha modes).")
			.Entry(AlphaMode::Opaque, "Opaque", "Alpha is ignored: the surface is fully opaque.")
			.Entry(AlphaMode::Mask, "Mask", "Pixels whose alpha is below AlphaCutoff are discarded; the others are opaque.")
			.Entry(AlphaMode::Blend, "Blend", "The surface is blended over what lies behind it by its alpha.");

		registry.Struct<MaterialData>("Material", "A PBR metallic-roughness material: a .material file, a glTF material or a built-in material.")
			.ColorField("BaseColor", &MaterialData::BaseColor, "The linear base colour and alpha, multiplied with BaseColorMap.", Utils::UnitRange())
			.Field("BaseColorMap", &MaterialData::BaseColorMap, "The sRGB base colour texture; null for none (White is bound).")
			.Field("Metallic", &MaterialData::Metallic, "How metallic the surface is: 0 dielectric, 1 metal; multiplied with the map's blue channel.",
				Utils::UnitRange())
			.Field("Roughness", &MaterialData::Roughness, "The perceptual roughness: 0 mirror-like, 1 fully rough; multiplied with the map's green channel.",
				Utils::UnitRange())
			.Field("MetallicRoughnessMap", &MaterialData::MetallicRoughnessMap,
				"The linear texture holding roughness in green and metalness in blue (glTF); null for none.")
			.Field("NormalMap", &MaterialData::NormalMap, "The tangent-space normal map; null for none (FlatNormal is bound).")
			.Field("NormalScale", &MaterialData::NormalScale, "The strength of the normal map's X and Y deflection.", Utils::NonNegative())
			.Field("OcclusionMap", &MaterialData::OcclusionMap, "The linear ambient occlusion texture (red channel); null for none.")
			.Field("OcclusionStrength", &MaterialData::OcclusionStrength, "How much the occlusion map darkens ambient light: 0 none, 1 fully.",
				Utils::UnitRange())
			.ColorField("Emissive", &MaterialData::Emissive, "The linear emitted colour, multiplied with EmissiveMap and EmissiveStrength.",
				Utils::UnitRange())
			.Field("EmissiveStrength", &MaterialData::EmissiveStrength, "The emission multiplier for HDR emission (KHR_materials_emissive_strength).",
				Utils::NonNegative())
			.Field("EmissiveMap", &MaterialData::EmissiveMap, "The sRGB emission texture; null for none (Black is bound).")
			.Field("AlphaMode", &MaterialData::AlphaMode, "How the base colour alpha is used.")
			.Field("AlphaCutoff", &MaterialData::AlphaCutoff, "The alpha below which Mask discards a pixel.", Utils::UnitRange())
			.Field("DoubleSided", &MaterialData::DoubleSided, "Whether back faces are drawn (and lit with flipped normals).")
			.Field("UVScale", &MaterialData::UVScale, "The scale applied to texture coordinates before UVOffset (KHR_texture_transform).")
			.Field("UVOffset", &MaterialData::UVOffset, "The offset added to scaled texture coordinates (KHR_texture_transform).");
	}

	std::vector<AssetHandle> GetMaterialTextureHandles(const MaterialData& material)
	{
		const std::array<AssetHandle, 5> slots = {
			material.BaseColorMap.GetHandle(),
			material.MetallicRoughnessMap.GetHandle(),
			material.NormalMap.GetHandle(),
			material.OcclusionMap.GetHandle(),
			material.EmissiveMap.GetHandle(),
		};
		std::vector<AssetHandle> handles;
		for (const AssetHandle handle : slots)
		{
			if (handle.IsValid())
				handles.push_back(handle);
		}
		std::ranges::sort(handles);
		const auto [first, last] = std::ranges::unique(handles);
		handles.erase(first, last);
		return handles;
	}

	Result<Json> MaterialToJson(const MaterialData& material, const TypeRegistry& registry)
	{
		ENGINE_TRY_ASSIGN(const StructInfo* type, Utils::FindMaterialType(registry));
		ENGINE_TRY_ASSIGN(Json fields, type->ToJson(&material));

		// The header first, then the fields in registry order (§6).
		Json document = Json::object();
		document["Format"] = std::string(MaterialData::FormatName);
		document["Version"] = MaterialData::CurrentVersion;
		for (auto field = fields.begin(); field != fields.end(); ++field)
			document[field.key()] = std::move(*field);
		return document;
	}

	Result<std::string> MaterialToText(const MaterialData& material, const TypeRegistry& registry, JsonStyle style)
	{
		ENGINE_TRY_ASSIGN(const Json document, MaterialToJson(material, registry));
		return JsonWriter::Write(document, style);
	}

	Result<MaterialData> MaterialFromJson(const Json& document, const TypeRegistry& registry, MaterialLoadReport& report, bool strictUnknowns)
	{
		report = MaterialLoadReport{};
		return Utils::ReadMaterialDocument(document, registry, report, strictUnknowns);
	}

	Result<MaterialData> MaterialFromText(std::string_view text, const TypeRegistry& registry, MaterialLoadReport& report, bool strictUnknowns)
	{
		report = MaterialLoadReport{};
		ENGINE_TRY_ASSIGN(const Json document, JsonReader::Parse(text));
		return MaterialFromJson(document, registry, report, strictUnknowns);
	}

	Result<Buffer> CookMaterial(const MaterialData& material, const TypeRegistry& registry, uint32_t importerVersion)
	{
		ENGINE_TRY_ASSIGN(const std::string text, MaterialToText(material, registry, JsonStyle::Minified));
		return WriteCookedArtifact(AssetType::Material, MaterialData::FormatVersion, importerVersion, AsBytes(text));
	}

	Result<AssetRef<MaterialData>> LoadCookedMaterial(std::span<const std::byte> cooked, const TypeRegistry& registry)
	{
		ENGINE_TRY_ASSIGN(const CookedArtifactView view, ReadCookedArtifact(cooked, AssetType::Material, MaterialData::FormatVersion));
		MaterialLoadReport report;
		ENGINE_TRY_ASSIGN(MaterialData material, MaterialFromText(AsStringView(view.Payload), registry, report, true));
		return AssetRef<MaterialData>(CreateRef<MaterialData>(std::move(material)));
	}

}
