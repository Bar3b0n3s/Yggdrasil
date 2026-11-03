#pragma once

#include "Shared/ShaderTypes.h"

#if !defined(ENGINE_SHADER)
	#include <cstddef>
	#include <cstdint>

namespace Engine {

#endif

	// The draw flags of an instance (InstanceRecord::Flags, Shared/InstanceRecord.h; DrawConstants::Flags before M23).
	// DrawFlagMirrored: the world matrix mirrors (a negative determinant), so the mesh's counter-clockwise front faces turn
	// clockwise on screen: the draw uses the CullFront (or, double-sided, CullNone) pipelines, and the shaders negate the
	// normal matrix (the cofactor of World, which det < 0 turns inwards) and the bitangent sign, and take SV_IsFrontFace
	// inverted. DrawFlagReceiveShadows: the forward passes sample the view's shadow maps for the instance's pixels.
#if defined(ENGINE_SHADER)
	static const uint DrawFlagMirrored = 1;
	static const uint DrawFlagReceiveShadows = 2;
#else
inline constexpr uint32_t DrawFlagMirrored = 1;
inline constexpr uint32_t DrawFlagReceiveShadows = 2;
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

	// The per-draw push constants of the Scene and Shadow programs' mesh pipelines (§8.4), version 2 (M23, C23.1, R-INST;
	// Docs/Decisions/0034-m23-contract.md decision 2): FirstInstance is the index of the draw's first instance record in
	// the view's Instances structured buffer (t10 of set 0, Shared/InstanceRecord.h), and the vertex shader reads instance i
	// of the draw (its SV_VulkanInstanceID, counted from 0: every draw's own first instance stays 0) at Instances[FirstInstance
	// + i]. The instance's world matrix, pick id and draw flags, which version 1 held here, are the record's. Declared in
	// Slang as [[vk::push_constant]] ConstantBuffer<DrawConstants> Draw.
	struct DrawConstants
	{
		uint32_t FirstInstance = 0;
		uint32_t Padding0 = 0;
		UInt2 Padding1;
	};

#if !defined(ENGINE_SHADER)
	static_assert(sizeof(DrawConstants) == 16, "DrawConstants must match its 16-byte push-constant layout (§8.4)");
	static_assert(offsetof(DrawConstants, Padding0) == 4 && offsetof(DrawConstants, Padding1) == 8,
		"DrawConstants members must sit where the push-constant layout puts them");

}
#endif
