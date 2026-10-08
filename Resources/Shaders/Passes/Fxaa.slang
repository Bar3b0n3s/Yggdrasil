// Fxaa: §8.3 pass 12, FXAA 3.11 (Renderer/FxaaPass.h).
// Contract stub of the M8 contract (Docs/Decisions/0013-m8-decisions.md decision 3): the program, its entry points and
// permutations are declared in Shaders.json; stream C replaces this body with the real shader and its C++ pass
// adds the pipeline layout description that "Shaders: LayoutsMatchReflection" checks.

[shader("compute")]
[numthreads(8, 8, 1)]
void CSMain(uint3 threadID : SV_DispatchThreadID)
{
}
