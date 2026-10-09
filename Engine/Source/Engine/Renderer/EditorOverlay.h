#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Renderer/RenderSnapshot.h"

namespace Engine {

	class AssetManager;

	// Pure renderer-side glyph/grid tessellation. Main-thread-independent: appends only to the caller-owned output.
	// No Scene, ImGui, font or Editor/Icons dependency. World glyphs are procedural line primitives; M10 UI glyphs are
	// independently drawn by Editor/Icons.cpp. Grid lies on XZ, emphasizes world axes, adapts spacing in powers of 10,
	// fades with distance, and uses snapshot camera/frustum in both projections (no wall clock).
	// EditorOverlays gates Grid/Icons; no camera -> no glyph/grid output. Icons at/behind camera are skipped.
	// InvalidArgument for invalid camera/icon values; builds before append so failure leaves output unchanged.
	// Reuses DebugDrawList's command budget; no geometry is written to the scene or a persistent debug list.
	[[nodiscard]] Status AppendEditorOverlay(const RenderSnapshot& snapshot, DebugDrawList& output);

	// Main thread, resolves CPU meshes through assets (same placeholder as rendering); no scene or GPU required.
	// Produces all three edges of each nondegenerate triangle in canonical snapshot/submesh/triangle order; shared
	// edges may repeat. No hidden-line removal here: caller draws depth-tested LineList at one framebuffer pixel,
	// matching solid reverse-Z prepass; white display color. CPU backface rejection matches material DoubleSided and
	// mirrored winding. Mask edges use geometric coverage (no sampled alpha); Blend edges are included and tested
	// against opaque/mask depth, without writing depth. Solid EntityId coverage remains unchanged, including mask holes.
	// Uses DebugDrawList command budget and reports dropped commands; no silent truncation, no polygon-line capability.
	// InvalidArgument for invalid camera/world data leaves output unchanged; budget drops are reported, not errors.
	// No camera, non-Lit or Wireframe off: no output.
	[[nodiscard]] Status AppendWireframeOverlay(const RenderSnapshot& snapshot, AssetManager& assets, DebugDrawList& output);

}
