// Skybox: §8.3 pass 8 (Renderer/SkyboxPass.h): the skybox cube at depth 0 along each pixel's view ray.
// Contract stub of the M8 contract (Docs/Decisions/0013-m8-decisions.md decision 3): the program, its entry points and
// permutations are declared in Shaders.json; stream B replaces this body with the real shader and its C++ pass
// adds the pipeline layout description that "Shaders: LayoutsMatchReflection" checks.

#include "Shared/EnvironmentConstants.h"

import Common.Environment;

[shader("vertex")]
float4 VSMain(uint vertexID : SV_VulkanVertexID) : SV_Position
{
	return float4(0.0, 0.0, 0.0, 1.0);
}

// Uses the frozen Common/Environment.slang functions and the Slang side of Shared/EnvironmentConstants.h, so every build
// compiles and validates them (slangc, spirv-val) before stream B's shader imports them for real; it binds nothing, so the
// stub's layout description stays valid.
[shader("fragment")]
float4 PSMain() : SV_Target0
{
	const EnvironmentConstants environment = (EnvironmentConstants)0;
	const float3 direction = RotateEnvironmentDirection(float3(0.0, 0.0, -1.0), environment.RotationSin, environment.RotationCos);
	return float4(EvaluateIrradianceSH9(environment.IrradianceSH9, direction), 0.0);
}
