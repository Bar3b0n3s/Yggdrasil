// The prepass's motion vectors and specular inputs (M26, C26.1, R-TAA-VEL, R-SSR-IN; Docs/Decisions/0037-m26-contract.md
// decisions 11 and 14; Renderer/SceneTargetFormats.h), shared by the passes that write DepthNormal's final targets: the
// mesh prepass and picking prepass (Passes/Scene.slang) and the terrain's (TerrainDepthNormal).
//   - SceneVelocity: the pixel's screen-space motion since the previous rendered frame in scene pixels (+x right, +y down),
//     current minus previous position: the surface point's current clip position (the vertex's SV_Position, passed on as
//     a varying of its own) and its previous one (the previous world position through
//     ViewConstants::PreviousViewProjection, which the renderer jitters by the current frame's jitter as it jitters
//     ViewProjection), both interpolated to the fragment and projected to pixels, so the jitter cancels (both positions
//     are moved by it) and a surface whose previous world and view-projection equal its current ones, computed by the same
//     NoContraction products, has exactly zero motion.
//   - SceneRoughness: two words of four unorm8 values each (x in the lowest byte), PackUnorm4x8.
module Motion;

// The scene pixel of clip position `clip` in a viewport of `viewportSize` pixels: x right from the left edge, y down from the
// top edge (clip-space +y is up, §8.3). A clip position at or behind the eye (w <= 0) has no pixel: it gives `fallback`.
public float2 ClipToScenePixel(float4 clip, float2 viewportSize, float2 fallback)
{
	if (!(clip.w > 0.0))
		return fallback;
	const float2 ndc = clip.xy / clip.w;
	return float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5) * viewportSize;
}

// The motion vector of a fragment whose surface point is at clip position `currentClip` and was at `previousClip` in the
// previous frame (both interpolated to the fragment): the current pixel minus the previous one; zero when either position is
// at or behind the eye, or the difference is not finite.
public float2 ComputeMotionVector(float4 currentClip, float4 previousClip, float2 viewportSize)
{
	const float2 current = ClipToScenePixel(currentClip, viewportSize, float2(0.0));
	const float2 motion = current - ClipToScenePixel(previousClip, viewportSize, current);
	return currentClip.w > 0.0 && all(isfinite(motion)) ? motion : float2(0.0);
}

// Four values, each saturated to [0, 1], packed into one word as unorm8, x in the lowest byte, each rounded to nearest.
public uint PackUnorm4x8(float4 values)
{
	const uint4 bytes = uint4(saturate(values) * 255.0 + 0.5);
	return bytes.x | (bytes.y << 8) | (bytes.z << 16) | (bytes.w << 24);
}

// The four unorm8 values of `word`, PackUnorm4x8's inverse up to its rounding.
public float4 UnpackUnorm4x8(uint word)
{
	return float4(uint4(word, word >> 8, word >> 16, word >> 24) & 0xFFu) / 255.0;
}
