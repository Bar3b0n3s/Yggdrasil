#pragma once

#include "Shared/ShaderTypes.h"

#if !defined(ENGINE_SHADER)
namespace Engine {

#endif

	struct SceneDataViewConstants
	{
		UInt4 Options; // RenderDebugView, remaining words zero
	};

#if !defined(ENGINE_SHADER)
	static_assert(sizeof(SceneDataViewConstants) == 16);

}
#endif
