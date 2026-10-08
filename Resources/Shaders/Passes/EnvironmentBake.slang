// EnvironmentBake: the GPU environment bake of §8.6 (Renderer/EnvironmentBaker.h): equirectangular to
// cube, cube downsampling, the GGX specular prefilter and the SH9 projection and reduction.
// Contract stub of the M8 contract (Docs/Decisions/0013-m8-decisions.md decision 3): the program, its entry points and
// permutations are declared in Shaders.json; stream B replaces this body with the real shader and its C++ pass
// adds the pipeline layout description that "Shaders: LayoutsMatchReflection" checks.

[shader("compute")]
[numthreads(8, 8, 1)]
void CSEquirectToCube(uint3 threadID : SV_DispatchThreadID)
{
}

[shader("compute")]
[numthreads(8, 8, 1)]
void CSDownsampleCube(uint3 threadID : SV_DispatchThreadID)
{
}

[shader("compute")]
[numthreads(8, 8, 1)]
void CSPrefilterSpecular(uint3 threadID : SV_DispatchThreadID)
{
}

[shader("compute")]
[numthreads(8, 8, 1)]
void CSProjectIrradiance(uint3 threadID : SV_DispatchThreadID)
{
}

[shader("compute")]
[numthreads(8, 8, 1)]
void CSReduceIrradiance(uint3 threadID : SV_DispatchThreadID)
{
}
