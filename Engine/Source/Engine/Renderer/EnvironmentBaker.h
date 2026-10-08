#pragma once

#include "Engine/Asset/EnvironmentData.h"
#include "Engine/Asset/IEnvironmentBaker.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Graphics/PipelineFactory.h"

#include <cstdint>
#include <vector>

// The GPU environment bake (Architecture §8.6), the Renderer's IEnvironmentBaker: EnvironmentImporter (AssetPipeline) calls
// it at import time in an editor with a device, and the result is cooked into the .envmap payload (EnvironmentData.h), so
// neither the editor at load nor the Runtime ever re-bakes. Steps, all compute on the graphics queue, then one readback:
//   1. the importer decodes the source (stbi_loadf to RGB32F) and rejects non-finite texels; the baker applies the optional
//      ClampLuminance of the input (texels whose Rec. 709 luminance exceeds ClampLuminanceMax scaled down to it);
//   2. the equirectangular image (uploaded as RGBA32F) to the EnvSkybox cube, RGBA16F, face max(1, min(1024, width / 4))
//      (or EnvironmentBakeOptions::SkyboxFaceSize), then its full mip chain by 2x2 box downsampling (skybox and
//      SkyboxBlur);
//   3. the specular prefilter into the EnvSpecular cube, RGBA16F, SpecularFaceSize² x SpecularMipCount mips, mip i for
//      perceptual roughness i / (SpecularMipCount - 1) (alpha = roughness²). Mip 0 (roughness 0) is the skybox resampled
//      to SpecularFaceSize: bilinear at each texel's direction from the smallest skybox level whose face is at least
//      SpecularFaceSize, or from level 0 when the skybox is smaller (a 64x32 source gives a 16² skybox; a 3000-wide one a
//      750² skybox with no 256 level), so it is a copy when a level has exactly that size. The other mips are made
//      by GGX importance sampling (N = V = R) on the Hammersley sequence with filtered importance sampling (Krivanek and
//      Colbert 2008: each sample reads the skybox mip chosen by its PDF and solid angle), from MinSamples samples at mip 1
//      rising linearly to MaxSamples at the last mip. This tames Poly Haven sun texels (> 50,000) without ad-hoc clamping;
//   4. the diffuse irradiance as SH9: a compute reduction over a skybox mip with each texel's solid angle, convolved with the
//      clamped cosine, divided by pi and windowed with a Hanning window against ringing from small bright sources (the
//      coefficient convention of EnvironmentData.h).
// Cube faces, texel directions and the equirectangular mapping follow EnvironmentData.h. The CPU references of step 3 (at
// 16²) and step 4 are in Tests/Source/Support/RenderReference.h, and the GPU oracle tests compare them: the furnace tests
// (a constant environment prefilters to 1 +- 0.5% in every mip; its SH irradiance is constant), "prefilter vs CPU at 16²" and
// the cube-seam continuity of step 2.
//
// Deterministic for a device class (no temporal noise, fixed sample sequences), so a cooked bake is reproducible where it
// was made; bakes are not compared across devices. Main thread only (it records and submits GPU work and waits for its
// readback, §4.11), so EnvironmentImporter runs on the main thread (IAssetImporter::RequiresMainThread). Not copyable or
// movable. Frozen by the M8 contract (Docs/Decisions/0013-m8-decisions.md decision 9).

namespace Engine {

	class GraphicsDevice;

	// Sizes and sample counts of a bake. The defaults are §8.6's, which IEnvironmentBaker::Bake uses; the oracle tests use
	// smaller cubes (the CPU prefilter reference at 16²).
	struct EnvironmentBakeOptions
	{
		// The skybox cube's face size; 0 chooses max(1, min(EnvironmentData::MaxSkyboxFaceSize, input width / 4)). Otherwise a
		// power of two from 4 to EnvironmentData::MaxSkyboxFaceSize (1024, what ValidateEnvironmentData accepts).
		uint32_t SkyboxFaceSize = 0;
		// The specular cube: a power of two from 4 to 1024, and 2 to log2(size) + 1 mips.
		uint32_t SpecularFaceSize = EnvironmentData::SpecularFaceSize;
		uint32_t SpecularMipCount = EnvironmentData::SpecularMipCount;
		// Importance samples per texel of specular mip 1 and of the last mip (linear in between); 1 to 4096, Min <= Max.
		uint32_t MinSamples = 64;
		uint32_t MaxSamples = 512;
	};

	class EnvironmentBaker final : public IEnvironmentBaker
	{
	public:
		// Equirect-to-cube, cube downsample, specular prefilter, SH projection and SH reduction.
		static constexpr uint32_t PipelineCount = 5;

		// Restricts construction to Create; CreateScope still reaches the constructor.
		class ConstructionKey
		{
			ConstructionKey() = default;
			friend class EnvironmentBaker;
		};

		// Use Create.
		explicit EnvironmentBaker(ConstructionKey key);
		~EnvironmentBaker() override;

		EnvironmentBaker(const EnvironmentBaker&) = delete;
		EnvironmentBaker& operator=(const EnvironmentBaker&) = delete;

		// Creates the bake's pipelines and samplers (at editor startup, with the editor's other pipelines, §8.12). `device`
		// is a documented back-reference that outlives the baker. Errors: those of PipelineFactory and the GraphicsDevice
		// wrappers (Gpu: FatalError(OutOfMemory) for a caller at startup, §8.14 item 7).
		[[nodiscard]] static Result<Scope<EnvironmentBaker>> Create(GraphicsDevice& device, PipelineFactory& pipelines);

		[[nodiscard]] static std::vector<PipelineLayoutDescription> GetLayoutDescriptions();
		[[nodiscard]] uint32_t GetPipelineCount() const;

		// BakeWithOptions with the default options (§8.6). Errors: as BakeWithOptions.
		[[nodiscard]] Result<EnvironmentData> Bake(const EnvironmentBakeInput& input) override;

		// The skybox cube, the prefiltered specular cube and the SH9 irradiance of `input` (steps 2 to 4), read back into an
		// EnvironmentData. Blocks until the GPU work is done (Readback, WaitForSubmission bounded like every engine wait).
		// The transient GPU objects are released before it returns. Errors: InvalidArgument for a malformed input (Width !=
		// 2 * Height, a zero size, a texel count that does not match, a non-finite or negative value) or options outside their
		// ranges; Gpu for a device failure, naming the resource.
		[[nodiscard]] Result<EnvironmentData> BakeWithOptions(const EnvironmentBakeInput& input, const EnvironmentBakeOptions& options);
	private:
		// The back-reference, the pipelines and the samplers (EnvironmentBaker.cpp).
		struct State;
	private:
		Scope<State> m_State;
	};

}
