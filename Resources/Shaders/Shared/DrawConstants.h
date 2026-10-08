#pragma once

#include "Shared/ShaderTypes.h"

#if !defined(ENGINE_SHADER)
	#include <cstddef>
	#include <cstdint>

namespace Engine {

#endif

	// DrawConstants::Flags bits. DrawFlagMirrored: the world matrix mirrors (a negative determinant), so the mesh's
	// counter-clockwise front faces turn clockwise on screen: the draw uses the CullFront (or, double-sided, CullNone)
	// pipelines, and the shaders negate the normal matrix (the cofactor of World, which det < 0 turns inwards) and the
	// bitangent sign, and take SV_IsFrontFace inverted.
#if defined(ENGINE_SHADER)
	static const uint DrawFlagMirrored = 1;
#else
inline constexpr uint32_t DrawFlagMirrored = 1;
#endif

	// The Scene program's debug views (§8.5): Vulkan specialization constant SceneDebugViewConstantId of the forward
	// pipelines, whose value is RenderDebugView's enumerator value (Renderer/RenderSnapshot.h; SceneRenderer.cpp asserts the
	// correspondence). Lit is the shaded image; the others output one quantity per pixel.
#if defined(ENGINE_SHADER)
	static const uint SceneDebugViewConstantId = 0;
	static const uint SceneDebugViewLit = 0;
	static const uint SceneDebugViewAlbedo = 1;
	static const uint SceneDebugViewNormals = 2;
	static const uint SceneDebugViewRoughness = 3;
	static const uint SceneDebugViewMetallic = 4;
	static const uint SceneDebugViewEmissive = 5;
#else
inline constexpr uint32_t SceneDebugViewConstantId = 0;
inline constexpr uint32_t SceneDebugViewLit = 0;
inline constexpr uint32_t SceneDebugViewAlbedo = 1;
inline constexpr uint32_t SceneDebugViewNormals = 2;
inline constexpr uint32_t SceneDebugViewRoughness = 3;
inline constexpr uint32_t SceneDebugViewMetallic = 4;
inline constexpr uint32_t SceneDebugViewEmissive = 5;
#endif

	// The per-draw push constants of every pipeline that draws meshes (§8.4): the item's world matrix (its rendered,
	// interpolated pose; the shaders derive the normal matrix from its cofactor), the entity id of the pick buffer (the
	// snapshot-local index + 1; the EntityId target arrives with M9) and the DrawFlag bits above. Declared in Slang as
	// [[vk::push_constant]] ConstantBuffer<DrawConstants> Draw.
	struct DrawConstants
	{
		Float4x4 World;
		uint32_t EntityId = 0;
		uint32_t Flags = 0;
		UInt2 Padding;
	};

#if !defined(ENGINE_SHADER)
	static_assert(sizeof(DrawConstants) == 80, "DrawConstants must match its 80-byte push-constant layout (§8.4)");
	static_assert(offsetof(DrawConstants, EntityId) == 64 && offsetof(DrawConstants, Padding) == 72,
		"DrawConstants members must sit where the push-constant layout puts them");

}
#endif
