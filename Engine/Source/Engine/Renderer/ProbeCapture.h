#pragma once

#include "Engine/Asset/IrradianceVolumeData.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Buffer.h"
#include "Engine/Core/Result.h"
#include "Engine/Renderer/RenderSnapshot.h"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

// Probe captures for lighting.bake (M26, C26.1, R-GI-PROBE; Docs/Decisions/0037-m26-contract.md decision 8): the GPU capture
// of a scene's radiance around a point into a cube, and the CPU steps that turn a capture into a probe's data (the SH9
// projection of an irradiance probe, the equirectangular image of a reflection probe's Environment). The bake itself, which
// renders, writes, imports and assigns, is EditorCore/Lighting/LightingBake.h.
//   - The capture renders `snapshot` six times through a SceneRenderer of FaceSize x FaceSize at the scene resolution, one
//     90-degree perspective camera per face of Vulkan's cube face order (EnvironmentData.h's face directions and up vectors,
//     as the point shadows' faces, M23), at `position`, with the snapshot's own quality but Probes false (one bounce: a bake
//     never sees an earlier bake), no particles, sprites, UI or overlays, and every temporal input reset, then reads the six
//     images back as linear RGBA32F texels, row 0 first, through SceneRenderer::GetSceneColorTexture.
//   - The scene's post-processing: the capture reads SceneColor, linear HDR radiance, before the HDR chain, so bloom,
//     auto-exposure, the tonemap, the LUT and FXAA never reach it. Two settings that write SceneColor earlier are turned
//     off: TAA (its resolve and jitter are a view's history) and SSR (W26.2's review): a view-dependent screen-space effect,
//     which in a 90-degree face traces only that face's image and fades at its edges, so it would seam the cube, add a
//     second specular bounce the views add again, and make a bake depend on a view toggle. GTAO (SsaoEnabled) is kept as
//     the scene sets it: its occlusion only darkens the ambient light in the creases a face sees, the geometry's own
//     occlusion the scene's views show too, so a bake keeps the look the scene is lit with. M32
//     (Docs/Decisions/0047-m32-fog-ssr.md decision 4): the scene's fog is off too (RenderSnapshot::Fog), so a probe bakes
//     the light alone and the views fog it where they shade it, never twice. M42 (Docs/Decisions/0059-m42-planar-reflections.md
//     decision 3): so are the planar reflections (RenderSnapshot::PlanarReflections), one bounce as the probes' term, so a
//     mirror bakes as its material draws without them. M47 (Docs/Decisions/0065-m47-water.md decision 3): so are the waters
//     (RenderSnapshot::Waters), as the fog is: the sea is out of probe bakes, and a bake under water sees the seabed dry.
//   - Deterministic for a device class, as the environment bake is (no temporal noise, no atomics deciding a sum or an
//     order): two captures of one snapshot give equal texels, and the CPU steps below are pure with IEEE-exact operations,
//     sqrt and DetMath only, so two bakes of one scene write byte-identical files (the acceptance "two probe bakes are
//     byte-identical").
//   - M45 (Docs/Decisions/0063-m45-sky-and-time-of-day.md decision 3, "Relit probes"): a relight capture (ProbeCaptureMode::
//     Relight) of MakeRelightCaptureSnapshot's snapshot, the scene under an enabled Sky with the sky dark and its Sun and Moon
//     lights off, also reads back each face's sky visibility (1 where SceneDepth holds the cleared far plane: no geometry hides
//     the sky; transparent surfaces write no depth, so a window sees it) and the sky's radiance totals, the projection of the
//     image-based light the capture's renderer made from the snapshot's sky (before its Intensity, so the dark sky does not
//     change them). ProjectCubeToIrradianceSH9 of such a capture is the probe's local light and ProjectCubeToSkyTransfer its sky
//     transfer (Renderer/ProbeRelight.h relights them).
//
// Frozen by the M26 contract C26.1; stream Q-A implements it; M45 adds the relight capture.

namespace Engine {

	class AssetManager;
	class GpuResourceCache;
	class GraphicsDevice;
	class SceneRendererPipelines;

	// One captured cube: six faces of FaceSize^2 linear radiance texels (alpha 1), faces in Vulkan's cube face order, each
	// row-major from its top row.
	struct ProbeCubeCapture
	{
		uint32_t FaceSize = 0;
		std::array<std::vector<glm::vec4>, 6> Faces{};
		// M45, a relight capture's (empty otherwise): each face's sky visibility in the faces' texel order, 1 where the face sees
		// the sky (no geometry's depth) and 0 elsewhere.
		std::array<std::vector<float>, 6> Visibility{};
		// M45, a relight capture's (empty otherwise): the radiance totals of the snapshot's sky (SceneRenderer::
		// GetSkyRadianceTotals), which MakeSkyRadianceSH9 (Renderer/ProbeRelight.h) turns into its radiance SH9.
		std::optional<std::array<glm::vec4, 10>> SkyRadianceTotals{};
	};

	// M45: what a capture reads back.
	enum class ProbeCaptureMode : uint8_t
	{
		Radiance, // the faces' radiance
		Relight,  // the faces' radiance, their sky visibility and the sky's radiance totals (the snapshot's Sky enabled)
	};

	// An equirectangular image of linear radiance, EnvironmentData.h's equirectangular mapping (what EnvironmentImporter
	// reads), row 0 at the top (+Y).
	struct ProbeEquirectImage
	{
		uint32_t Width = 0;
		uint32_t Height = 0;
		std::vector<glm::vec3> Texels{};
	};

	// The irradiance probe of `capture`: its radiance projected onto the real SH basis with each texel's exact solid angle,
	// convolved with the clamped cosine, divided by pi and windowed (EnvironmentData's convention, so a capture of uniform
	// radiance L gives coefficients that evaluate to L in every direction: the furnace). Pure; IEEE-exact operations, sqrt and
	// DetMath only (each texel's solid angle through DetMath::ATan2), in a fixed order (faces, then rows, then columns), so it
	// is identical in every configuration.
	[[nodiscard]] std::array<glm::vec3, 9> ProjectCubeToIrradianceSH9(const ProbeCubeCapture& capture);

	// M45: the sky transfer of a relight capture (its Visibility of FaceSize^2 values a face, asserted): T_ij, the sum over its
	// texels of V Y_i Y_j times the texel's exact solid angle, as ProjectCubeToIrradianceSH9 sums (the same basis, solid angles
	// and order, in double), as IrradianceProbeRelight::Transfer holds it (the upper triangle, row by row). A capture that sees
	// the sky everywhere gives about the identity, one that sees it nowhere zero. Pure.
	[[nodiscard]] std::array<float, IrradianceTransferCount> ProjectCubeToSkyTransfer(const ProbeCubeCapture& capture);

	// M45: the snapshot a relight capture renders (decision 3): `source` with its Sky's Intensity 0 (the sky pass draws black
	// and the sky's image-based light is zero) and its Sun's and Moon's lights' colours 0 (they light nothing, while the sky
	// still scatters their authored illuminance into the radiance its totals hold), so a capture of it sees the scene's own
	// lights alone. `source` without an enabled Sky is returned unchanged. Pure.
	[[nodiscard]] RenderSnapshot MakeRelightCaptureSnapshot(const RenderSnapshot& source);

	// The reflection probe's image of `capture`: 4 FaceSize x 2 FaceSize texels, each the bilinear sample of the cube in its
	// direction (EnvironmentData.h's mapping, directions through DetMath, so identical in every configuration). Pure.
	[[nodiscard]] ProbeEquirectImage ResampleCubeToEquirect(const ProbeCubeCapture& capture);

	// `image` as a Radiance HDR file (RGBE, the "#?RADIANCE" header, uncompressed scanlines), deterministic byte for byte: the
	// `.hdr` lighting.bake writes for a reflection probe, which EnvironmentImporter imports. Pure.
	[[nodiscard]] Buffer WriteRadianceHdr(const ProbeEquirectImage& image);

	// The GPU capture (see the file comment). Owns one SceneRenderer, resized to each capture's face size (its previous size's
	// targets released first, as ViewportCapture's), and the readback staging; registered with the device's recreation
	// registry through its renderer (a SceneRenderer, the placement rule's place 2) and as "ProbeCapture" for its command list
	// and staging (released and created again with it; its TrimUnused hook releases the renderer's targets between captures),
	// so a capture after a device loss's recovery gives the texels it gave before. Main thread only; not copyable or movable.
	class ProbeCapture
	{
	public:
		// Restricts construction to Create; CreateScope still reaches the constructor.
		class ConstructionKey
		{
			ConstructionKey() = default;
			friend class ProbeCapture;
		};

		// Use Create.
		explicit ProbeCapture(ConstructionKey key);
		~ProbeCapture();

		ProbeCapture(const ProbeCapture&) = delete;
		ProbeCapture& operator=(const ProbeCapture&) = delete;

		// `device`, `pipelines`, `cache` and `assets` are documented back-references that outlive the capture. Errors: those of
		// SceneRenderer::Create.
		[[nodiscard]] static Result<Scope<ProbeCapture>> Create(GraphicsDevice& device, SceneRendererPipelines& pipelines, GpuResourceCache& cache,
			AssetManager& assets);

		// The cube of `snapshot`'s radiance around `position` (world space), `faceSize` texels a face (a power of two in
		// [16, 512]). Blocks until the readback is done (bounded like every engine wait). Before it returns, it collects the
		// garbage of its completed submissions (GraphicsDevice::CollectCompletedSubmissions; World Tree's brief 21), so what its
		// faces used and dropped (each face's read-back staging) is released with it rather than at the frame's garbage
		// collection, and any number of captures in one frame hold one capture's device allocations. M45: with
		// ProbeCaptureMode::Relight it also reads back each face's sky visibility from SceneDepth and, after the first face, the
		// sky's radiance totals. Errors: InvalidArgument for a face size out of range, a non-finite position, or a relight
		// capture of a snapshot without an enabled Sky; those of SceneRenderer::Render and of the readback (Gpu, also when a
		// relight capture's renderer made no sky lighting; on a lost device the loss).
		[[nodiscard]] Result<ProbeCubeCapture> Capture(const RenderSnapshot& snapshot, const glm::vec3& position, uint32_t faceSize,
			ProbeCaptureMode mode = ProbeCaptureMode::Radiance);
	private:
		// The renderer, the back-references and the staging textures (ProbeCapture.cpp).
		struct State;
	private:
		Scope<State> m_State;
	};

}
