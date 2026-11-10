#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Renderer/RenderSnapshot.h"
#include "Engine/Scene/SceneRaycast.h"

#include <optional>
#include <span>
#include <string>
#include <vector>

// 2D picking (M21, C21.1, R-2D-EDITOR; Docs/Decisions/0032-m21-contract.md decision 14): the sprite a view pixel shows, on
// the CPU, without a GPU or a physics world. Sprites draw in painter's order without writing depth, so the sprite a pixel
// shows is the last one drawn (highest sorting layer, then order in layer, then canonical order) whose quad covers it; the
// GPU pick reads only the meshes' EntityId. The editor's click selection and viewport.pick ask PickSprite first and fall back
// to the mesh pick when it hits no sprite (a sprite therefore wins over a mesh in front of it: an accepted limit of scenes
// that mix them, recorded in Backlog §6). The quads are SpriteGeometry's (Asset/SpriteGeometry.h) at the rendered pose, so
// what is drawn is what is picked; texture alpha is not sampled (a transparent texel inside the quad is still the sprite).
//
// Frozen by the M21 contract (C21.1); stream A implements it.

namespace Engine {

	class AssetManager;
	class Scene;

	struct SpritePickRequest
	{
		RenderRay Ray{};
		float MaxDistance = 1000.0f; // positive, finite, metres along the normalized ray
		float MinDistance = 0.0f;    // finite, in [0, MaxDistance)
		float Alpha = 1.0f;          // [0, 1], the rendered pose; callers refresh the scene's transforms as for extraction
		// The project's SortingLayers.Layers (RenderExtractionRequest::SortingLayers).
		std::vector<std::string> SortingLayers{};
	};

	// The last-drawn effectively enabled sprite whose quad (two-sided) the ray crosses within [MinDistance, MaxDistance],
	// as a SceneRaycastHit: Entity, Distance, Position, Normal (the quad's, against the ray), Submesh 0, Triangle 0 or 1 (the
	// quad's two triangles, top-left first) and its Barycentric weights. A sprite is hit on the quad it is drawn with
	// (SpriteRendererComponent.h's rule): a texture that is missing or fails to load is hit on its placeholder's quad, and a
	// sheet that is missing or fails to load, or an out-of-range Frame, gives no quad and is never hit. Empty optional: no
	// sprite hit. Errors: InvalidArgument for a non-finite or zero direction, a non-finite origin, an invalid distance
	// interval or Alpha. Main thread. M21 (C21.2, Docs/Decisions/0032-m21-contract.md decision 24, stream T1): the
	// TilemapRenderers take part as the sprites they draw: a ray that crosses a drawn tile's quad hits the tilemap's entity,
	// in the one draw order of sprites and tilemaps (TilemapItem::DrawPosition), the last-drawn tile of a tilemap first. M21's
	// milestone review: a tilemap's tiles are tested only in the chunks whose bounds hold the point where the ray meets the
	// tilemap's plane, so a pick over a large tilemap tests the tiles under the ray, not every tile, with the same result.
	[[nodiscard]] Result<std::optional<SceneRaycastHit>> PickSprite(const Scene& scene, AssetManager& assets, const SpritePickRequest& request);

	// PickSprite against `sprites`, a snapshot's sprites in their draw order (RenderSnapshot::Sprites: effectively enabled,
	// at the pose and in the sorting layers that snapshot was extracted with), so a caller picks in the image it rendered
	// whatever the scene has done since: the editor's click selection keeps its displayed image's sprites for a click
	// (EditorHostViewports). The request's Alpha and SortingLayers are the snapshot's own and are not read; its ray and
	// distances are checked as PickSprite checks them. Errors: PickSprite's. Main thread.
	[[nodiscard]] Result<std::optional<SceneRaycastHit>> PickSprite(std::span<const SpriteItem> sprites, AssetManager& assets, const SpritePickRequest& request);

	// M21 (C21.2, decision 24, stream T1): PickSprite against a snapshot's sprites and tilemaps (RenderSnapshot::Sprites and
	// Tilemaps, each tilemap drawn before Sprites[DrawPosition]), so the editor's click selection picks a tile of the image it
	// rendered as it picks a sprite: a hit tile is its tilemap's entity. The sprites-only overload is this one with no
	// tilemaps. Errors: PickSprite's. Main thread.
	[[nodiscard]] Result<std::optional<SceneRaycastHit>> PickSprite(std::span<const SpriteItem> sprites, std::span<const TilemapItem> tilemaps,
		AssetManager& assets, const SpritePickRequest& request);

}
