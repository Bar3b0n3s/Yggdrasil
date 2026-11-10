#pragma once

#include "Engine/Asset/AssetHandle.h"
#include "Engine/Asset/SpriteSheetData.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/UUID.h"
#include "Engine/Scene/SpriteValidation.h"

#include <cstdint>
#include <string>
#include <string_view>

// The clip a SpriteAnimator names, looked up in its SpriteRenderer's sprite sheet, and the SPRITE_UNKNOWN_CLIP finding when
// there is none (M21, Docs/Decisions/0032-m21-contract.md decisions 5 and 12): the one rule the SpriteAnimation phase
// (SpriteAnimationSystem.cpp) and project.validate's clip check (FindSpriteSceneIssues, SpriteValidation.cpp) share. Main
// thread, as the asset manager it loads through.

namespace Engine {

	class AssetManager;

	namespace Detail {

		// What a clip name resolves to.
		enum class SpriteClipStatus : uint8_t
		{
			Found,            // the sheet loads and has the clip
			NoSheet,          // the SpriteRenderer's Sheet is null
			SheetUnavailable, // the sheet is missing, fails to load or is not a sprite sheet
			UnknownClip       // the sheet loads but has no clip of that name
		};

		struct SpriteClipLookup
		{
			SpriteClipStatus Status = SpriteClipStatus::NoSheet;
			AssetRef<SpriteSheetData> Sheet{}; // the loaded sheet (Found, UnknownClip); keeps Clip alive
			const SpriteClip* Clip = nullptr;  // the clip, inside Sheet (Found)
			std::string SheetPath{};           // the sheet's reference path (every status but NoSheet)
			std::string LoadError{};           // why the sheet is unavailable (SheetUnavailable)
		};

		// The clip `clip` (not empty) of the sheet `sheet` (null: NoSheet), loaded through `assets` (AssetManager::Load).
		[[nodiscard]] SpriteClipLookup FindSpriteClip(AssetManager& assets, AssetHandle sheet, std::string_view clip);

		// SPRITE_UNKNOWN_CLIP (Warning, SpriteAnimator.Clip) for the SpriteAnimator of `entity`, at `entityPath`, whose clip
		// `clip` resolved to `lookup` (any status but Found).
		[[nodiscard]] SpriteSceneIssue MakeUnknownClipIssue(UUID entity, std::string_view entityPath, std::string_view clip, const SpriteClipLookup& lookup);

	}

}
