#include "EnginePCH.h"
#include "Engine/Scene/Private/SpriteClipLookup.h"

#include "Engine/Asset/AssetManager.h"

#include <algorithm>
#include <format>
#include <string>

namespace Engine {

	namespace Detail {

		SpriteClipLookup FindSpriteClip(AssetManager& assets, AssetHandle sheet, std::string_view clip)
		{
			SpriteClipLookup lookup;
			if (!sheet.IsValid())
				return lookup;
			lookup.SheetPath = assets.GetReferencePath(sheet);
			Result<AssetRef<Asset>> loaded = assets.Load(sheet);
			if (!loaded)
			{
				lookup.Status = SpriteClipStatus::SheetUnavailable;
				lookup.LoadError = loaded.error().ToString();
				return lookup;
			}
			lookup.Sheet = AssetCast<SpriteSheetData>(*loaded);
			if (lookup.Sheet == nullptr)
			{
				lookup.Status = SpriteClipStatus::SheetUnavailable;
				lookup.LoadError = std::format("it is a {}, not a SpriteSheet", AssetTypeToString((*loaded)->GetAssetType()));
				return lookup;
			}
			const auto found = std::ranges::find(lookup.Sheet->Clips, clip, &SpriteClip::Name);
			if (found == lookup.Sheet->Clips.end())
			{
				lookup.Status = SpriteClipStatus::UnknownClip;
				return lookup;
			}
			lookup.Status = SpriteClipStatus::Found;
			lookup.Clip = &*found;
			return lookup;
		}

		SpriteSceneIssue MakeUnknownClipIssue(UUID entity, std::string_view entityPath, std::string_view clip, const SpriteClipLookup& lookup)
		{
			SpriteSceneIssue issue;
			issue.Code = std::string(SpriteUnknownClipCode);
			issue.Severity = DiagnosticSeverity::Warning;
			issue.Entity = entity;
			issue.Component = "SpriteAnimator";
			issue.Field = "Clip";
			switch (lookup.Status)
			{
				case SpriteClipStatus::NoSheet:
					issue.Message = std::format("the SpriteAnimator of '{}' names the clip '{}', but its SpriteRenderer has no sprite sheet; it plays nothing",
						entityPath, clip);
					issue.Hint = "set the SpriteRenderer's Sheet to a sprite sheet with the clip, or clear Clip";
					return issue;
				case SpriteClipStatus::SheetUnavailable:
					issue.Message = std::format("the SpriteAnimator of '{}' names the clip '{}' of the sprite sheet '{}', which does not load ({}); it plays nothing",
						entityPath, clip, lookup.SheetPath, lookup.LoadError);
					issue.Hint = "restore or repair the sprite sheet, or point the SpriteRenderer's Sheet at another one";
					return issue;
				case SpriteClipStatus::Found:
				case SpriteClipStatus::UnknownClip:
					break;
			}
			issue.Message = std::format("the SpriteAnimator of '{}' names the clip '{}', which its sprite sheet '{}' does not have; it plays nothing",
				entityPath, clip, lookup.SheetPath);
			issue.Hint = "name a clip of the sheet, add the clip to the sheet, or clear Clip";
			return issue;
		}

	}

}
