// Bloom: §8.3 pass 10 (Renderer/BloomPass.h): the 13-tap downsample with the Karis average into mip 0,
// the plain downsample and the 3x3 tent upsample; permutation BLOOM_RGBA16 selects the storage format.
// Contract stub of the M8 contract (Docs/Decisions/0013-m8-decisions.md decision 3): the program, its entry points and
// permutations are declared in Shaders.json; stream C replaces this body with the real shader and its C++ pass
// adds the pipeline layout description that "Shaders: LayoutsMatchReflection" checks.

[shader("compute")]
[numthreads(8, 8, 1)]
void CSDownsampleKaris(uint3 threadID : SV_DispatchThreadID)
{
}

[shader("compute")]
[numthreads(8, 8, 1)]
void CSDownsample(uint3 threadID : SV_DispatchThreadID)
{
}

[shader("compute")]
[numthreads(8, 8, 1)]
void CSUpsample(uint3 threadID : SV_DispatchThreadID)
{
}
