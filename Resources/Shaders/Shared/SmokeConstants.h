#pragma once

// Shared between C++ and Slang (Architecture §8.4): the common subset of both languages, with ENGINE_SHADER
// guarding what only one side sees. slangc predefines __SLANG__; C++ compilers never do.
#if defined(__SLANG__) && !defined(ENGINE_SHADER)
	#define ENGINE_SHADER 1
#endif

#if !defined(ENGINE_SHADER)
	#include <cstdint>

namespace Engine {

#endif

	// Constants of the Smoke compute program (Passes/Smoke.slang), which scales a buffer of floats. It exists so that
	// the shader pipeline (Shaders project, CompileShaders.py, slangc, spirv-val) is exercised end to end.
	struct SmokeConstants
	{
		uint32_t ElementCount = 0;
		float Scale = 1.0f;
		uint32_t Reserved0 = 0;
		uint32_t Reserved1 = 0;
	};

#if !defined(ENGINE_SHADER)
	static_assert(sizeof(SmokeConstants) == 16, "SmokeConstants must match the 16-byte constant buffer layout");

}
#endif
