#pragma once

#include "Engine/Asset/AssetType.h"
#include "Engine/Asset/TypedAssetHandle.h"
#include "Engine/Core/Base.h"

namespace Engine {

	// Registry name "TilemapCollider" (Architecture §5.3; M21, C21.2, R-2D-TILE; Docs/Decisions/0032-m21-contract.md decision
	// 25): the solid cells of a Tilemap asset (Asset/TilemapData.h: the non-empty cells whose palette entry is Solid) as 2D
	// colliders. Requires Transform. A 2D collider like BoxCollider2D, CircleCollider2D and PolygonCollider2D, under the same
	// composition rules (Scene/Physics2DSystem.h): without a RigidBody2D on the entity or above it, its boxes form the implicit
	// static body, one static compound in the 2D world; with one, they join that body's compound.
	//   - The boxes: each chunk's solid cells merged into rectangles (MergeTilemapChunkBoxes, Asset/TilemapCollision.h), never
	//     across a chunk's edge, chunks in the tilemap's canonical order, so the shape is built in canonical order and editing
	//     one chunk changes only its boxes. Cell (x, y) spans [x CellSize.x, (x + 1) CellSize.x] x [y CellSize.y,
	//     (y + 1) CellSize.y] in the entity's local XY plane, the cells the TilemapRenderer draws its tiles centred on; the
	//     entity's world scale applies in X and Y. Every box is a sub-shape of this collider: contacts, queries and
	//     physics2d.bodyInfo name the tilemap's entity. A body that rolls across boxes' seams sets its RigidBody2D's
	//     EnhancedInternalEdgeRemoval (§9.2 "Seams").
	//   - Tilemap names the asset; null takes the entity's TilemapRenderer.Tilemap (as a MeshCollider without a Mesh takes its
	//     MeshRenderer's). A tilemap that is missing or fails to load, an entity with neither, or more than
	//     MaxTilemapColliderBoxes boxes is PHYSICS_INVALID_SHAPE (its body is not created); a tilemap without a solid cell adds
	//     no shape, and a body left without any is not created, without a diagnostic (an authoring state, as a RigidBody2D
	//     without colliders). A new asset version of the tilemap (an edit through tilemap.paint, a reimport) rebuilds the
	//     body's shape in place at the next Physics2DPreStep.
	//   - IsTrigger makes the boxes a sensor, as a 2D collider's.
	// Its body is an ordinary 2D body: the physics2d state-hash block covers it like any other, and a scene without a
	// TilemapCollider makes no body, loads no tilemap and hashes exactly as before.
	struct TilemapColliderComponent
	{
		TypedAssetHandle<AssetType::Tilemap> Tilemap{};
		bool IsTrigger = false;
	};

}
