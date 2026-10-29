#pragma once

#include "Shared/ShaderTypes.h"

#if !defined(ENGINE_SHADER)
	#include <cstddef>
	#include <cstdint>

namespace Engine {

#endif

	// One instance of an instanced mesh draw (M23, C23.1, R-INST; Docs/Decisions/0034-m23-contract.md decision 2): an entry
	// of the view's Instances structured buffer (t10 of descriptor set 0), which every mesh pipeline's vertex shader reads at
	// the draw's first instance plus its instance index (DrawConstants, stream A). The Prepare stage writes one per mesh draw
	// item and submesh draw of the view, from the snapshot's MeshDrawItem:
	//   - World: the item's rendered world matrix (MeshDrawItem::World);
	//   - PreviousWorld: the item's world matrix at the previous frame's render, for motion vectors (M26's TAA, plan §4.1):
	//     MeshDrawItem::PreviousWorld, or World when the item has none (no motion), which is every item until M26;
	//   - EntityId: what the picking prepass writes into EntityId (MeshDrawItem::PickId: 0 not pickable);
	//   - Flags: the DrawFlag bits of Shared/DrawConstants.h (DrawFlagMirrored, DrawFlagReceiveShadows), which therefore
	//     move from the per-draw push constants into the record.
	// The members are ordered so that every Float4x4 starts at a 16-byte boundary and the size is a multiple of 16, which
	// makes the C++ layout equal the std430 layout of a Vulkan structured buffer.
	struct InstanceRecord
	{
		Float4x4 World;
		Float4x4 PreviousWorld;
		uint32_t EntityId = 0;
		uint32_t Flags = 0;
		uint32_t Padding0 = 0;
		uint32_t Padding1 = 0;
	};

#if !defined(ENGINE_SHADER)
	static_assert(sizeof(InstanceRecord) == 144, "InstanceRecord must match its 144-byte structured-buffer layout");
	static_assert(offsetof(InstanceRecord, PreviousWorld) == 64 && offsetof(InstanceRecord, EntityId) == 128,
		"InstanceRecord members must sit where the structured-buffer layout puts them");

}
#endif
