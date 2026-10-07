#pragma once

#include "Shared/ShaderTypes.h"

#if !defined(ENGINE_SHADER)
namespace Engine {

#endif

	// The push constants of the ImGui program (Passes/ImGui.slang, §8.11): the orthographic projection of ImGui's display
	// rectangle into clip space, clip = position * Scale + Translate.
	struct ImGuiConstants
	{
		Float2 Scale;
		Float2 Translate;
	};

#if !defined(ENGINE_SHADER)
	static_assert(sizeof(ImGuiConstants) == 16, "ImGuiConstants must match its 16-byte push-constant layout");

}
#endif
