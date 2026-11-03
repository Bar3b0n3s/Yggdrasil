#pragma once

#include "Engine/Core/Result.h"
#include "Engine/Renderer/DebugDrawList.h"
#include "Engine/Renderer/RenderSnapshot.h"

// The scene view's 2D grid (M21, R-2D-EDITOR; Docs/Decisions/0032-m21-contract.md decision 14): in the 2D mode the grid lies
// in the plane z = 0, where sprites and 2D bodies live, instead of the renderer's ground grid in y = 0 (EditorOverlay's
// RenderViewFlags::Grid, which a camera looking down -Z sees edge-on). The scene view's host leaves the Grid flag off in
// the 2D mode and appends these lines to the snapshot's DebugDraw, which the renderer draws in the same Overlays pass as
// its own grid, depth-tested against the meshes; sprites write no depth, so the grid shows over them.

namespace Engine {

	// Appends the grid lines of the plane z = 0 that `camera` sees, by the renderer's grid rules turned into that plane: a
	// cell spans a tenth of the view's scale (the orthographic half height, or the camera's distance from the plane in
	// perspective), rounded up to a power of ten; at most GridHalfLines2D cells on each side of the view axis's point on the
	// plane; each line in segments that fade with their distance from that point. The X axis (y = 0) is red, the Y axis
	// (x = 0) green, every other line grey. Appends nothing when the camera sees no part of the plane (a view along it).
	// Errors: InvalidArgument for a camera whose frustum overflows world coordinates (output unchanged). Pure, main thread.
	[[nodiscard]] Status AppendEditorGrid2D(const CameraData& camera, DebugDrawList& output);

	// The most cells the 2D grid covers on each side of its centre.
	inline constexpr double GridHalfLines2D = 100.0;

}
