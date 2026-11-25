#pragma once

#include "Shared/ShaderTypes.h"

#if !defined(ENGINE_SHADER)
	#include <cstddef>
	#include <cstdint>

namespace Engine {

#endif

	// One instance of an instanced mesh draw (M23, C23.1, R-INST; Docs/Decisions/0034-m23-contract.md decision 2): an entry
	// of the view's Instances structured buffer (t10 of descriptor set 0 of the Scene, Shadow and SelectionMask programs),
	// which every mesh pipeline's vertex shader reads at the draw's first instance plus its instance index (DrawConstants
	// version 2, SelectionMaskConstants). The Prepare stage writes one per submesh draw of the view, from the snapshot's
	// MeshDrawItem: the camera's opaque draws in their order, its transparent draws in theirs, the shadow casters' draws
	// (ShadowPass::PrepareCasters) and the selection mask's (SelectionPass::PrepareDraws), so the instances of every
	// instanced draw are adjacent; RecordViewSetup writes them once per view. Its members:
	//   - World: the item's rendered world matrix (MeshDrawItem::World);
	//   - PreviousWorld: the item's world matrix at the previous frame's render, for motion vectors (M26's TAA, plan §4.1):
	//     MeshDrawItem::PreviousWorld, or World when the item has none (no motion), which is every item until M26;
	//   - EntityId: what the picking prepass writes into EntityId (MeshDrawItem::PickId: 0 not pickable);
	//   - Flags: the DrawFlag bits of Shared/DrawConstants.h (DrawFlagMirrored, DrawFlagReceiveShadows), which therefore
	//     move from the per-draw push constants into the record;
	//   - M31 (Docs/Decisions/0046-m31-material-parameters.md decision 4): the item's material parameters
	//     (MeshDrawItem::Parameters, the MaterialParameters component as extraction resolved it), which the fragment shaders
	//     of the Scene program and of the Shadow program's Mask casters read only for an instance with
	//     DrawFlagMaterialParameters, so an instance without it draws exactly as before M31: Opacity (the dithered fade, 1
	//     opaque), DissolveAmount (0 none), Tint (multiplies the base colour), Emissive (radiance added to the material's
	//     emission), DissolveEdge (the radiance added along the dissolve's edge) with DissolveEdgeWidth, DissolveNoiseScale
	//     (the dissolve noise's repeats per unit of the first texture coordinates) and UVOffset (added to every map's
	//     texture coordinates after the material's UV transform, the scroll included, in [0, 1)). The neutral values are
	//     MeshMaterialParameters' defaults (Renderer/RenderSnapshot.h).
	//   - M46 (Docs/Decisions/0064-m46-mesh-lods-draw-distance-and-wind.md decisions 3E and 3G; in Padding1 and Padding2, the
	//     size unchanged): LodFade, the draw's signed level fade (Renderer/MeshLod.h's MeshLodDraw::Fade, Common/LodFade.slang),
	//     0 for none, which the Mask variants of the camera's passes clip by and a Blend material's alpha follows; and
	//     WindResponse, with DrawFlagWind, its material's wind block as four unorm8 bytes (Renderer/MeshWind.h's
	//     PackWindResponse: Bend, Branch, Flutter and Stiffness over their ranges, Shared/WindConstants.h), which every vertex
	//     entry's displacement reads (Common/Wind.slang); 0 and unread without the flag.
	// The members are ordered so that every Float4x4 and Float3 starts at a 16-byte boundary, every Float2 at an 8-byte one,
	// and the size is a multiple of 16, which makes the C++ layout equal the std430 layout of a Vulkan structured buffer.
	struct InstanceRecord
	{
		Float4x4 World;
		Float4x4 PreviousWorld;
		uint32_t EntityId = 0;
		uint32_t Flags = 0;
		float Opacity = 1.0f;
		float DissolveAmount = 0.0f;
		Float3 Tint = Float3(1.0f, 1.0f, 1.0f);
		float DissolveEdgeWidth = 0.0f;
		Float3 Emissive = Float3(0.0f, 0.0f, 0.0f);
		float DissolveNoiseScale = 1.0f;
		Float3 DissolveEdge = Float3(0.0f, 0.0f, 0.0f);
		// M42 (Docs/Decisions/0059-m42-planar-reflections.md decision 3; in Padding0, the size unchanged): with
		// DrawFlagPlanarReflection, the slot (0 to MaxPlanarReflectionsPerView - 1) of the view's reflection the instance
		// samples; 0 and unread without the flag.
		uint32_t PlanarReflectionSlot = 0;
		Float2 UVOffset = Float2(0.0f, 0.0f);
		float LodFade = 0.0f;
		uint32_t WindResponse = 0;
	};

	// M43 (Docs/Decisions/0061-m43-static-scenery.md decision 3): one entry of a view's InstanceIndices structured buffer (t22
	// of set 0 of the Scene and Shadow programs): a static draw (DrawConstants::Flags with DrawConstantsStaticInstances)
	// reads instance i's record at StaticInstances[InstanceIndices[FirstInstance + i].Record], StaticInstances (t21) being the
	// GPU-resident records of the scene's static set. Reserved is 0 and unread at M43, kept for M46's per-view fade of a
	// static entry (ADR 0060's ledger), so M46 changes no layout of M43's. M46 (W46.4): Reserved holds the bits of the entry's
	// signed level fade (InstanceRecord::LodFade's meaning, 0 for none), which Common/Instances.slang's LoadInstanceRecord
	// returns in the static record, since a static record is shared by every view.
	struct InstanceIndex
	{
		uint32_t Record = 0;
		uint32_t Reserved = 0;
	};

#if !defined(ENGINE_SHADER)
	static_assert(sizeof(InstanceIndex) == 8 && offsetof(InstanceIndex, Reserved) == 4, "InstanceIndex must match its 8-byte structured-buffer layout");
	static_assert(sizeof(InstanceRecord) == 208, "InstanceRecord must match its 208-byte structured-buffer layout");
	static_assert(offsetof(InstanceRecord, PreviousWorld) == 64 && offsetof(InstanceRecord, EntityId) == 128 && offsetof(InstanceRecord, Tint) == 144
			&& offsetof(InstanceRecord, Emissive) == 160 && offsetof(InstanceRecord, DissolveEdge) == 176 && offsetof(InstanceRecord, UVOffset) == 192
			&& offsetof(InstanceRecord, LodFade) == 200 && offsetof(InstanceRecord, WindResponse) == 204,
		"InstanceRecord members must sit where the structured-buffer layout puts them");

}
#endif
