#pragma once

#include "Shared/ShaderTypes.h"

#if !defined(ENGINE_SHADER)
namespace Engine {

#endif

	// The per-material constants: b0 of descriptor set 1 (§8.4), one buffer per (material, version). The walking skeleton's
	// renderer reads only the base colour factor (linear RGBA, MaterialData::BaseColor); the PBR renderer (M8) adds the
	// other factors and the texture slots of set 1.
	struct MaterialConstants
	{
		Float4 BaseColor;
	};

#if !defined(ENGINE_SHADER)
	static_assert(sizeof(MaterialConstants) == 16, "MaterialConstants must match its 16-byte constant-buffer layout");

}
#endif
