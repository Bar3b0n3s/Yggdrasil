#include "EnginePCH.h"
#include "Engine/Asset/DocumentData.h"

#include "Engine/Asset/CookedFormat.h"
#include "Engine/Core/Assert.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/Json/JsonWriter.h"

#include <nlohmann/json.hpp>

#include <format>

namespace Engine {

	namespace Utils {

		// The canonical document of a cooked JSON asset of type T, whose "Format" must be `format`.
		template<typename T>
		static Result<AssetRef<T>> LoadCookedDocument(std::span<const std::byte> cooked, std::string_view format)
		{
			ENGINE_TRY_ASSIGN(const CookedArtifactView view, ReadCookedArtifact(cooked, T::StaticType, T::FormatVersion));
			ENGINE_TRY_ASSIGN(Json document, JsonReader::Parse(AsStringView(view.Payload)));
			const JsonReader root(document);
			ENGINE_TRY(root.ExpectType(JsonType::Object));
			ENGINE_TRY_ASSIGN(const JsonReader formatMember, root.GetMember("Format"));
			ENGINE_TRY_ASSIGN(const std::string actualFormat, formatMember.ReadString());
			if (actualFormat != format)
			{
				return std::unexpected(formatMember.MakeLocatedError(ErrorCode::Validation,
					std::format("the cooked {} holds a \"{}\" document, not a \"{}\"", AssetTypeToString(T::StaticType), actualFormat, format)));
			}
			Ref<T> asset = CreateRef<T>();
			asset->Document = CreateRef<const Json>(std::move(document));
			return AssetRef<T>(std::move(asset));
		}

	}

	Result<Buffer> CookDocument(AssetType type, const Json& document, uint32_t importerVersion)
	{
		ENGINE_CORE_ASSERT(type == AssetType::Scene || type == AssetType::Prefab, "CookDocument cooks scenes and prefabs, not {}",
			AssetTypeToString(type));
		ENGINE_TRY_ASSIGN(const std::string text, JsonWriter::Write(document, JsonStyle::Minified));
		const uint16_t formatVersion = type == AssetType::Scene ? SceneData::FormatVersion : PrefabData::FormatVersion;
		return WriteCookedArtifact(type, formatVersion, importerVersion, AsBytes(text));
	}

	Result<AssetRef<SceneData>> LoadCookedScene(std::span<const std::byte> cooked)
	{
		return Utils::LoadCookedDocument<SceneData>(cooked, "Scene");
	}

	Result<AssetRef<PrefabData>> LoadCookedPrefab(std::span<const std::byte> cooked)
	{
		return Utils::LoadCookedDocument<PrefabData>(cooked, "Prefab");
	}

}
