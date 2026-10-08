#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Graphics/PipelineFactory.h"
#include "Engine/Renderer/PassBindingCache.h"
#include "Engine/Renderer/RenderSnapshot.h"

#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <vector>

// §8.3 pass 11, tonemap and encode (Architecture §8.9): one compute pass from SceneColor (RGBA16_FLOAT, linear radiance) to
// LdrColor (RGBA8_UNORM, display-encoded values):
//   1. bloom (§8.3 pass 10): lerp(radiance, bloom, BloomIntensity) with the bloom chain's mip 0 sampled bilinearly at full
//      resolution, when TonemapPassInputs::Bloom is set;
//   2. exposure: times ViewConstants::Exposure (2^ExposureEV);
//   3. the tonemapper, on c = max(exposed radiance, 0) per channel, giving display-linear values in [0, 1] (the formulas
//      below, which Common/Tonemapping.slang and Tests/Source/Support/RenderReference.h both implement from this text; the
//      GPU test "TonemapPass: every tonemapper matches its CPU reference at 1,024 points" compares them);
//   4. the sRGB OETF in the shader (Common/Color.slang), unless EncodeSrgb is false (the data debug views);
//   5. blue-noise triangular dither of +-1 LSB before the 8-bit quantization (no sky banding), from the 64x64 R8 blue-noise
//      texture tiled over the image by pixel coordinate (no temporal variation, §8.3), when Dither is set and BlueNoise is
//      given; a pixel's dither depends only on its coordinates, so a render is reproducible.
// Alpha is 1. The pass reads ViewConstants (b0: ViewportSize, Exposure), SceneColor (t0) and writes LdrColor (u0,
// [vk::image_format("rgba8")]); the bloom chain, the blue noise and the pass's own settings are bound as C decides.
//
// The tonemappers (frozen by the M8 contract so the shader and the CPU reference agree; M = 3x3 matrices applied as
// out = M * in to the column (r, g, b), written row by row):
//   - AgX, Base look (Sobotka's AgX as Blender 4 configures it, in the widespread "minimal" fit for Rec. 709 primaries):
//       1. inset: v = M_in * c with M_in rows (0.842479062253094, 0.0784335999999992, 0.0792237451477643),
//          (0.0423282422610123, 0.878468636469772, 0.0791661274605434), (0.0423756549057051, 0.0784336, 0.879142973793104);
//       2. log2 encoding: x = clamp((log2(max(v, 1e-10)) - MinEv) / (MaxEv - MinEv), 0, 1) with MinEv = -12.47393 and
//          MaxEv = 4.026069;
//       3. the sigmoid as its 6th-order polynomial fit: s = 15.5 x^6 - 40.14 x^5 + 31.96 x^4 - 6.868 x^3 + 0.4298 x^2
//          + 0.1191 x - 0.00232;
//       4. outset: o = M_out * s with M_out rows (1.19687900512017, -0.0980208811401368, -0.0990297440797205),
//          (-0.0528968517574562, 1.15190312990417, -0.0989611768448433), (-0.0529716355144438, -0.0980434501171241,
//          1.15107367264116);
//       5. to display-linear: clamp(pow(max(o, 0), 2.2), 0, 1) (AgX's 2.2 power display EOTF; the sRGB OETF of step 4 of
//          the pass then encodes it).
//   - ACES (Hill's fit of the RRT and sRGB ODT, no exposure pre-scale): v = M_aces_in * c with rows (0.59719, 0.35458,
//     0.04823), (0.07600, 0.90834, 0.01566), (0.02840, 0.13383, 0.83777); w = (v (v + 0.0245786) - 0.000090537) /
//     (v (0.983729 v + 0.4329510) + 0.238081) per channel; clamp(M_aces_out * w, 0, 1) with rows (1.60475, -0.53108,
//     -0.07367), (-0.10208, 1.10813, -0.00605), (-0.00327, -0.07276, 1.07602).
//   - Khronos PBR Neutral (the Khronos reference implementation): x = min(r, g, b); offset = x < 0.08 ? x - 6.25 x² : 0.04;
//     c -= offset; peak = max(r, g, b); if peak < StartCompression (0.76) the result is c; otherwise, with d = 0.24,
//     newPeak = 1 - d² / (peak + d - 0.76), c *= newPeak / peak, g = 1 - 1 / (0.15 (peak - newPeak) + 1) and the result is
//     lerp(c, (newPeak, newPeak, newPeak), g); finally clamped to [0, 1].
//   - Linear: clamp(c, 0, 1).
//
// Created once per device at startup and owned by SceneRendererPipelines (§8.12); any number of views record with it, one
// at a time. The pass keeps no binding sets: each Record takes them from the view's PassBindingCache (the SceneRenderer
// keeps one per pass with its targets). Main thread only; not copyable or movable. Frozen by the M8 contract
// (Docs/Decisions/0013-m8-decisions.md decision 8): stream C owns the implementation, which the contract moved here from
// the walking skeleton's SceneRenderer (exposure, Linear and the OETF only).

namespace Engine {

	class GraphicsDevice;

	// What the pass applies besides the exposure (which comes from ViewConstants::Exposure).
	struct TonemapSettings
	{
		RenderTonemapper Tonemapper = RenderTonemapper::AgX;
		// Blue-noise triangular dither of +-1 LSB before quantization; needs TonemapPassInputs::BlueNoise.
		bool Dither = true;
		// The sRGB OETF; false stores the tonemapped value v as round(255 v) (the data debug views, RenderSnapshot.h).
		bool EncodeSrgb = true;
	};

	// One record's resources. Every texture is in a shader-readable state between command lists (keepInitialState) except
	// LdrColor, which the pass writes as a storage image; SceneColor and LdrColor have the same size.
	struct TonemapPassInputs
	{
		nvrhi::IBuffer* ViewConstants = nullptr; // b0 ViewConstants (Shared/ViewConstants.h); never null
		nvrhi::ITexture* SceneColor = nullptr;   // SceneColorFormat; never null
		// Optional: the texture BloomPass::Record returned (its mip 0 holds the bloom); null applies no bloom.
		nvrhi::ITexture* Bloom = nullptr;
		float BloomIntensity = 0.0f; // PostProcessSettings::BloomIntensity, in [0, 1]
		// Optional: the blue-noise texture (BuiltinAssetHandles::BlueNoiseTexture, BlueNoise.h; R8_UNORM 64x64); null turns
		// the dither off.
		nvrhi::ITexture* BlueNoise = nullptr;
		nvrhi::ITexture* LdrColor = nullptr; // LdrColorFormat with isUAV; never null
		TonemapSettings Settings{};
	};

	class TonemapPass
	{
	public:
		// The pipelines Create makes (one compute pipeline; the tonemapper and flags are constants, not permutations).
		static constexpr uint32_t PipelineCount = 1;

		// Restricts construction to Create; CreateScope still reaches the constructor.
		class ConstructionKey
		{
			ConstructionKey() = default;
			friend class TonemapPass;
		};

		// Use Create.
		explicit TonemapPass(ConstructionKey key);
		~TonemapPass();

		TonemapPass(const TonemapPass&) = delete;
		TonemapPass& operator=(const TonemapPass&) = delete;

		// Creates the pipeline (checked against its reflection) and its samplers. `device` is a documented back-reference
		// that outlives the pass. Errors: those of PipelineFactory and the GraphicsDevice wrappers (Gpu: FatalError(OutOfMemory)
		// for a caller at startup, §8.14 item 7).
		[[nodiscard]] static Result<Scope<TonemapPass>> Create(GraphicsDevice& device, PipelineFactory& pipelines);

		// The layout description of every pipeline Create makes, for "Shaders: LayoutsMatchReflection".
		[[nodiscard]] static std::vector<PipelineLayoutDescription> GetLayoutDescriptions();

		// The pipelines Create made (PipelineCount once implemented).
		[[nodiscard]] uint32_t GetPipelineCount() const;

		// Records the pass over the whole of `inputs.LdrColor` into `commandList` (open, asserted), with binding sets from the
		// view's `bindings`. Asserts the required inputs and equal sizes. Errors: those of creating a binding set for new
		// resources (Gpu).
		[[nodiscard]] Status Record(nvrhi::ICommandList& commandList, PassBindingCache& bindings, const TonemapPassInputs& inputs);
	private:
		// The back-reference and the pipeline (TonemapPass.cpp).
		struct State;
	private:
		Scope<State> m_State;
	};

}
