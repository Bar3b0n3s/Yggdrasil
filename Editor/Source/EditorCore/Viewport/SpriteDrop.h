#pragma once

#include "Engine/Asset/AssetHandle.h"
#include "Engine/Asset/AssetType.h"
#include "Engine/Core/Json/Json.h"
#include "Engine/Core/Result.h"
#include "Engine/Renderer/RenderSnapshot.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <optional>

// Dropping sprites into the scene view (M21, C21.1, R-2D-EDITOR; Docs/Decisions/0032-m21-contract.md decision 14): a sprite
// sheet or a texture dragged from the content browser onto the scene view creates a sprite entity where the cursor's ray
// meets the plane z = 0 (the camera's target when the ray is parallel to it or meets it behind the camera), through
// entity.create, the drop's whole action, so automation reaches it without a method of its own (as a mesh drop's
// entity.create). SceneViewportPanel::AcceptAssetDrop (M21's file in phase 2, ADR 0028's ledger) builds the request with
// these functions; stream D implements them.

namespace Engine {

	// The point where `ray` (a unit direction) meets the plane z = 0, or nullopt when it is parallel to it or meets it behind
	// its origin. Pure.
	[[nodiscard]] std::optional<glm::vec3> IntersectSpritePlane(const RenderRay& ray);

	struct SpriteDropRequest
	{
		AssetHandle Asset{};              // the dropped asset, a registered SpriteSheet or Texture
		AssetType Type = AssetType::None; // its type
		glm::vec3 Position = glm::vec3(0.0f);
		bool PlayScene = false; // the scene view shows the play scene: target "play", else "edit"
		uint64_t Revision = 0;  // the editor's revision, sent as ifRevision
	};

	// The entity.create params of a drop: {name: "Sprite", target, ifRevision, components: {Transform: {Translation:
	// Position}, SpriteRenderer: {Sheet: Asset}}} for a sheet (its frame 0) or {SpriteRenderer: {Texture: Asset}} for a
	// texture. InvalidArgument for any other type or an invalid handle.
	[[nodiscard]] Result<Json> MakeSpriteDropParams(const SpriteDropRequest& request);

}
