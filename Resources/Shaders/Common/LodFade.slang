// The cross-fade of a mesh's levels of detail (M46, Docs/Decisions/0064-m46-mesh-lods-draw-distance-and-wind.md decision 3E;
// Renderer/MeshLod.h): an instance's signed InstanceRecord::LodFade t splits its pixels by Opacity's ordered-dither threshold
// (Scene.slang's GetFadeThreshold), so a coarser level fading in with t > 0 keeps the pixels whose threshold is below t and
// the finer level with -t keeps the others, exactly complementary, and a level fading out before its CullDistance keeps
// fewer of them as t rises. 0 keeps every pixel. The prepass, the forward passes and the picking prepass evaluate it in
// the Mask variants (GetCoverage), which an Opaque material takes while its draw fades (Renderer/RenderPrepare.h's
// SelectMeshAlphaMode), and a Blend material's alpha drops to 0 where it removes a pixel, so it composes with Opacity's
// clip by the same threshold.

// Whether the level fade `fade` removes the pixel of dither threshold `threshold`.
bool IsLodFaded(float fade, float threshold)
{
	if (fade > 0.0)
		return !(threshold < fade);
	if (fade < 0.0)
		return !(threshold >= -fade);
	return false;
}
