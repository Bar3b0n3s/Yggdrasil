#include "EnginePCH.h"
#include "Engine/Scene/SpriteValidation.h"

#include "Engine/Asset/AssetManager.h"
#include "Engine/Asset/SpriteSheetData.h"
#include "Engine/Scene/Components/RuntimeComponents.h"
#include "Engine/Scene/Components/SpriteAnimatorComponent.h"
#include "Engine/Scene/Components/SpriteRendererComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Private/SpriteClipLookup.h"
#include "Engine/Scene/Scene.h"

#include <algorithm>
#include <format>

namespace Engine {

	namespace Utils {

		// SPRITE_UNKNOWN_SORTING_LAYER and SPRITE_FRAME_OUT_OF_RANGE of `entity`'s SpriteRenderer `sprite`.
		static void FindSpriteRendererIssues(ConstEntity entity, const SpriteRendererComponent& sprite, AssetManager* assets,
			std::span<const std::string> sortingLayers, std::vector<SpriteSceneIssue>& issues)
		{
			if (std::ranges::find(sortingLayers, sprite.SortingLayer) == sortingLayers.end())
			{
				issues.push_back(SpriteSceneIssue{
					.Code = std::string(SpriteUnknownSortingLayerCode),
					.Severity = DiagnosticSeverity::Warning,
					.Entity = entity.GetUUID(),
					.Component = "SpriteRenderer",
					.Field = "SortingLayer",
					.Message = std::format("the sprite names the sorting layer '{}', which SortingLayers.Layers does not declare; it draws in the first layer",
						sprite.SortingLayer),
					.Hint = "Declare the layer with project.setSettings, or correct the name.",
				});
			}
			if (assets == nullptr || !sprite.Sheet.IsValid())
				return;
			// A sheet that is missing or fails to load is ASSET_MISSING or ASSET_IMPORT_FAILED, reported by the asset checks.
			const Result<AssetRef<Asset>> loaded = assets->Load(sprite.Sheet.GetHandle());
			const AssetRef<SpriteSheetData> sheet = loaded.has_value() ? AssetCast<SpriteSheetData>(*loaded) : nullptr;
			if (sheet == nullptr || sprite.Frame < sheet->Frames.size())
				return;
			issues.push_back(SpriteSceneIssue{
				.Code = std::string(SpriteFrameOutOfRangeCode),
				.Severity = DiagnosticSeverity::Warning,
				.Entity = entity.GetUUID(),
				.Component = "SpriteRenderer",
				.Field = "Frame",
				.Message = std::format("the sprite's Frame {} is not an index of sprite sheet '{}', which has {} frames; it draws nothing", sprite.Frame,
					assets->GetReferencePath(sprite.Sheet.GetHandle()), sheet->Frames.size()),
				.Hint = "Set Frame below the sheet's frame count, or add the frame to the sheet.",
			});
		}

		// SPRITE_UNKNOWN_CLIP (stream B): a SpriteAnimator whose Clip is not empty and names no clip of its SpriteRenderer's
		// sheet, when the sheet loads or there is none. A sheet that does not load is the asset checks' finding
		// (ASSET_MISSING, ASSET_TYPE_MISMATCH, ASSET_IMPORT_FAILED), not this code's.
		static void FindUnknownClip(const Scene& scene, ConstEntity entity, AssetManager& assets, std::vector<SpriteSceneIssue>& issues)
		{
			const SpriteAnimatorComponent* animator = entity.TryGetComponent<SpriteAnimatorComponent>();
			const SpriteRendererComponent* renderer = entity.TryGetComponent<SpriteRendererComponent>();
			if (animator == nullptr || renderer == nullptr || animator->Clip.empty())
				return;
			const Detail::SpriteClipLookup lookup = Detail::FindSpriteClip(assets, renderer->Sheet.GetHandle(), animator->Clip);
			if (lookup.Status == Detail::SpriteClipStatus::NoSheet || lookup.Status == Detail::SpriteClipStatus::UnknownClip)
				issues.push_back(Detail::MakeUnknownClipIssue(entity.GetUUID(), scene.GetEntityPath(entity), animator->Clip, lookup));
		}

	}

	std::vector<SpriteSceneIssue> FindSpriteSceneIssues(const Scene& scene, AssetManager* assets, std::span<const std::string> sortingLayers)
	{
		std::vector<SpriteSceneIssue> issues;
		scene.ForEachCanonical([&scene, assets, sortingLayers, &issues](ConstEntity entity)
		{
			if (entity.HasComponent<HierarchyDisabledTag>())
				return;
			if (const SpriteRendererComponent* sprite = entity.TryGetComponent<SpriteRendererComponent>())
				Utils::FindSpriteRendererIssues(entity, *sprite, assets, sortingLayers, issues);
			if (assets != nullptr)
				Utils::FindUnknownClip(scene, entity, *assets, issues);
		});
		return issues;
	}

}
