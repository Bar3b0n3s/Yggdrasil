#include "EnginePCH.h"
#include "Engine/AssetPipeline/Importers/SceneImporter.h"

#include "Engine/Asset/DocumentData.h"
#include "Engine/AssetPipeline/Private/DocumentAssetReferences.h"
#include "Engine/Core/UUIDGenerator.h"
#include "Engine/Scene/LoadReport.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/SceneSerializer.h"

#include <nlohmann/json.hpp>

#include <array>
#include <utility>

namespace Engine {

	std::span<const std::string_view> SceneImporter::GetExtensions() const
	{
		static constexpr std::array<std::string_view, 1> Extensions = { ".scene" };
		return Extensions;
	}

	Result<ImportResult> SceneImporter::Import(ImportContext& context, const AssetMetadata& metadata) const
	{
		const std::string sourcePath = context.GetSourcePath().ToString();

		// The scratch scene only holds the file's entities; a strict load never generates IDs, so the generator's seed is
		// irrelevant to the output.
		UUIDGenerator idGenerator = UUIDGenerator::CreateDeterministic(0);
		SceneSpecification specification;
		specification.Name = std::string(context.GetSourcePath().GetStem());
		specification.Registry = &context.GetRegistry();
		specification.IdGenerator = &idGenerator;
		const Scope<Scene> scene = Scene::Create(specification);

		LoadOptions options;
		options.Mode = LoadMode::Strict;
		options.SourcePath = sourcePath;
		LoadReport report;
		ENGINE_TRY(WithContext(SceneSerializer::LoadFromString(*scene, AsStringView(context.GetSourceBytes()), options, report),
			std::format("while importing scene '{}'", sourcePath)));

		// Saved canonically at the current version: what the Runtime's strict load reads (§7.4).
		ENGINE_TRY_ASSIGN(const Json document, SceneSerializer::ToJson(*scene));
		ENGINE_TRY_ASSIGN(Buffer cooked, CookDocument(AssetType::Scene, document, Version));

		ImportResult result;
		result.Artifacts.push_back(ImportedArtifact{ .Handle = metadata.Handle, .Type = AssetType::Scene, .SubAssetKey = {}, .Cooked = std::move(cooked) });
		result.Dependencies = Utils::CollectDocumentAssetReferences(document, context.GetRegistry());
		return result;
	}

}
