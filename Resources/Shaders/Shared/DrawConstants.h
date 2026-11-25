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
	// DrawFlagPreviousPositions (M26, R-TAA-VEL; Docs/Decisions/0037-m26-contract.md decision 11): the prepass's motion
	// vectors take the instance's previous local positions from vertex binding 2 (a deformed item's previous deformation,
	// M23's Skinning pass) instead of its current ones; set only for a deformed item that continues its view's history.
	// DrawFlagMaterialParameters (M31; Docs/Decisions/0046-m31-material-parameters.md decision 4): the record's material
	// parameters are not neutral, so the fragment shaders read and apply them (MakeMeshInstanceRecord, Renderer/RenderPrepare.h);
	// without it they never read them and draw as before M31.
	// DrawFlagPlanarReflection (M42, Docs/Decisions/0059-m42-planar-reflections.md decision 3): the instance is a drawn
	// planar reflection's glass in its main view: the forward passes reflect the reflection of slot
	// InstanceRecord::PlanarReflectionSlot in place of the probe's or environment's specular, and the prepass writes a zero
	// SceneRoughness multiplier so SSR neither traces nor composites over it. A mirror view's records never carry it.
	// DrawFlagWind (M46, Docs/Decisions/0064-m46-mesh-lods-draw-distance-and-wind.md decision 3G): the instance's material has
	// wind enabled and the scene has a Wind, so every vertex entry displaces its vertices by the view's WindConstants and the
	// record's WindResponse (Common/Wind.slang); without it no entry reads either, and the instance draws as before M46.
#if defined(ENGINE_SHADER)
	static const uint DrawConstantsStaticInstances = 1; // M43: DrawConstants::Flags, not an InstanceRecord draw flag
	static const uint DrawConstantsWind = 2;            // M46: DrawConstants::Flags (W46.4)
	static const uint DrawFlagMirrored = 1;
	static const uint DrawFlagReceiveShadows = 2;
	static const uint DrawFlagPreviousPositions = 4;
	static const uint DrawFlagMaterialParameters = 8;
	static const uint DrawFlagPlanarReflection = 16;
	static const uint DrawFlagWind = 32;
#else
inline constexpr uint32_t DrawConstantsStaticInstances = 1; // M43: DrawConstants::Flags, not an InstanceRecord draw flag
inline constexpr uint32_t DrawConstantsWind = 2;            // M46: DrawConstants::Flags (W46.4)
inline constexpr uint32_t DrawFlagMirrored = 1;
inline constexpr uint32_t DrawFlagReceiveShadows = 2;
inline constexpr uint32_t DrawFlagPreviousPositions = 4;
inline constexpr uint32_t DrawFlagMaterialParameters = 8;
inline constexpr uint32_t DrawFlagPlanarReflection = 16;
inline constexpr uint32_t DrawFlagWind = 32;
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
	// M43 (Docs/Decisions/0061-m43-static-scenery.md decision 3; Padding0's place, the size unchanged): Flags, the draw's
	// DrawConstants bits. With DrawConstantsStaticInstances, the draw is a static draw: instance i is
	// StaticInstances[InstanceIndices[FirstInstance + i].Record] (t21 and t22 of set 0, Shared/InstanceRecord.h's
	// InstanceIndex) instead of Instances[FirstInstance + i]; 0 keeps every draw on the dynamic path.
	// M46 (Docs/Decisions/0064-m46-mesh-lods-draw-distance-and-wind.md, the W46.4 record; Padding1.x's place, the size
	// unchanged): with DrawConstantsWind, a static draw whose material has wind in a scene with a Wind, WindResponse is that
	// material's (InstanceRecord::WindResponse's packing): Common/Instances.slang's load of a static record ORs DrawFlagWind into
	// it and takes this WindResponse, since a static item's one record serves every submesh of it, whose materials' wind
	// differs (a tree's bark and leaves). 0 for every dynamic draw, whose records carry their own.
	struct DrawConstants
	{
		uint32_t FirstInstance = 0;
		uint32_t Flags = 0;
		uint32_t WindResponse = 0;
		uint32_t Padding1 = 0;
	};

#if !defined(ENGINE_SHADER)
	static_assert(sizeof(DrawConstants) == 16, "DrawConstants must match its 16-byte push-constant layout (§8.4)");
	static_assert(offsetof(DrawConstants, Flags) == 4 && offsetof(DrawConstants, WindResponse) == 8
			&& offsetof(DrawConstants, Padding1) == 12,
		"DrawConstants members must sit where the push-constant layout puts them");

}
#endif
