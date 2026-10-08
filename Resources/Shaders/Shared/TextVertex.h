#pragma once

#include "Shared/ShaderTypes.h"

#if !defined(ENGINE_SHADER)
	#include <cstdint>

namespace Engine {

#endif

	// How the Text program's vertex shader places a glyph vertex (Passes/Text.slang, Renderer/TextRenderer.h): the Mode
	// attribute of each vertex, with Position and Corner read as follows.
	//   Screen:    Position.xy is the vertex in normalized device coordinates (the CPU lays screen text out in pixels of
	//              the target); no camera is involved.
	//   World:     Position is the vertex in world space, projected through View then Projection.
	//   Billboard: Position is the text's world-space origin and Corner the vertex's offset from it in view space (metres,
	//              +x right, +y up), so the block stays parallel to the view plane with the camera's up as its up.
	//   Label:     Position is the label's world-space point and Corner the vertex's offset from the projected point in
	//              normalized device coordinates (+y up), scaled by the clip-space w so the label keeps its pixel size; a
	//              point on or behind the camera plane (view-space z >= 0) draws nothing.
#if defined(ENGINE_SHADER)
	static const uint TextVertexScreen = 0;
	static const uint TextVertexWorld = 1;
	static const uint TextVertexBillboard = 2;
	static const uint TextVertexLabel = 3;
#else
inline constexpr uint32_t TextVertexScreen = 0;
inline constexpr uint32_t TextVertexWorld = 1;
inline constexpr uint32_t TextVertexBillboard = 2;
inline constexpr uint32_t TextVertexLabel = 3;
#endif

#if !defined(ENGINE_SHADER)

}
#endif
