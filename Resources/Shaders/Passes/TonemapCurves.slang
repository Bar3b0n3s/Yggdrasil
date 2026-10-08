// TonemapCurves: the GPU side of "TonemapPass: every tonemapper matches its CPU reference at 1,024 points"
// (§8.9, §15.3): evaluates every tonemapper of Common/Tonemapping.slang at the values of a buffer. No engine pass uses it;
// its pipeline layout lives in Tests/Source/Support/TonemapCurvesProgram.h.
// Contract stub of the M8 contract (Docs/Decisions/0013-m8-decisions.md decision 3): the program, its entry points and
// permutations are declared in Shaders.json; stream C replaces this body with the real shader and its C++ pass
// adds the pipeline layout description that "Shaders: LayoutsMatchReflection" checks.

[shader("compute")]
[numthreads(8, 8, 1)]
void CSMain(uint3 threadID : SV_DispatchThreadID)
{
}
