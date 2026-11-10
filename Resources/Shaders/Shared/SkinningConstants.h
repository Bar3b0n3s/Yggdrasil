#pragma once

#include "Shared/ShaderTypes.h"

#if !defined(ENGINE_SHADER)
	#include <cstddef>
	#include <cstdint>

namespace Engine {

#endif

	// The push constants of one dispatch of the Skinning pass (M23, C23.3, C-SKEL, C-MORPH; Docs/Decisions/0034-m23-contract.md
	// decision 27; Renderer/SkinningPass.h, Passes/Skinning.slang): one deformed mesh item, one thread per vertex. Counts.x is
	// the vertex count; Counts.y the skin's joint count, 0 without skinning (no palette, or no skin); Counts.z the morph target
	// count, 0 without morphing; Counts.w the flags: SkinningFlagFirstDeform makes the dispatch write the new positions into
	// the previous positions too (the item's first deformation by its renderer, so its motion is zero). Every thread reads
	// only its own vertex and writes only its own outputs, so no atomic decides a value.
	//
	// A dispatch runs rows of at most SkinningRowGroups thread groups of 64 threads (SkinningPass::ThreadGroupSize) along x,
	// and as many rows along y as the vertex count needs: thread (x, y) deforms vertex y * SkinningRowGroups * 64 + x, so a
	// mesh of any vertex count stays within Vulkan's guaranteed 65,535 groups per dimension.
#if defined(ENGINE_SHADER)
	static const uint SkinningFlagFirstDeform = 1;
	static const uint SkinningRowGroups = 32768;
#else
inline constexpr uint32_t SkinningFlagFirstDeform = 1;
inline constexpr uint32_t SkinningRowGroups = 32768;
#endif

	struct SkinningConstants
	{
		UInt4 Counts; // vertices, skin joints, morph targets, flags
	};

#if !defined(ENGINE_SHADER)
	static_assert(sizeof(SkinningConstants) == 16, "SkinningConstants must match its 16-byte push-constant layout");
	static_assert(offsetof(SkinningConstants, Counts) == 0, "SkinningConstants members must sit where the push-constant layout puts them");

}
#endif
