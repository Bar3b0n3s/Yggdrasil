#pragma once

#include "Shared/ShaderTypes.h"

#if !defined(ENGINE_SHADER)
	#include <cstddef>
	#include <cstdint>

namespace Engine {

#endif

	// The per-draw push constants of every pipeline that draws meshes (§8.4): the item's world matrix (its rendered,
	// interpolated pose; the shaders derive the normal matrix from its cofactor), the entity id of the pick buffer (the
	// snapshot-local index + 1; the EntityId target arrives with M9) and flags (none are defined yet). Declared in Slang as
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
