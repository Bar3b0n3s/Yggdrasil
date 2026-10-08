// Text: §8.3 pass 14 (Renderer/TextRenderer.h): signed-distance-field glyph quads, smoothstep over
// fwidth.
// Contract stub of the M8 contract (Docs/Decisions/0013-m8-decisions.md decision 3): the program, its entry points and
// permutations are declared in Shaders.json; stream D replaces this body with the real shader and its C++ pass
// adds the pipeline layout description that "Shaders: LayoutsMatchReflection" checks.

[shader("vertex")]
float4 VSMain(uint vertexID : SV_VulkanVertexID) : SV_Position
{
	return float4(0.0, 0.0, 0.0, 1.0);
}

[shader("fragment")]
float4 PSMain() : SV_Target0
{
	return float4(0.0, 0.0, 0.0, 0.0);
}
