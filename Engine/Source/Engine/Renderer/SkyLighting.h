#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Graphics/PipelineFactory.h"
#include "Engine/Renderer/RenderSnapshot.h"
#include "Shared/SkyConstants.h"

#include <glm/glm.hpp>
#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

// The sky's lighting on the GPU (M45, Docs/Decisions/0063-m45-sky-and-time-of-day.md decision 3; Architecture §8.6): what a
// Lit view with a camera and an enabled Sky records in the Ambient stage's SkyLighting declaration, after its CPU constants and
// before the forward passes read them, and what its Forward stage's Sky pass samples (Renderer/SkyPass.h).
//   - SkyLuts: the transmittance and multiple-scattering LUTs when the atmosphere's key (SkyLutKey) changed or the view's
//     resources are new, then the camera's sky-view LUT every frame (Passes/Sky.slang, Common/Atmosphere.slang).
//   - SkyIbl: when the image-based light's key changed (SkyLightingKey), the update: the sea-level sky-view LUT, the cube's
//     level 0 (CSSkyCube: the atmosphere and the clouds at their stepped offset, no discs or stars, the ground below the
//     horizon), its mip chain, the GGX prefilter into the specular cube and the radiance SH9's projection and reduction, with
//     EnvironmentBake's CSDownsampleCube, CSPrefilterSpecular, CSProjectIrradiance and CSReduceIrradiance and their layouts
//     (EnvironmentBaker::GetLayoutDescriptions), the same SPIR-V, so every import bake stays byte-identical. Then, every frame,
//     CSSkyFinalize (the irradiance SH9 times Intensity and the fog's tinted colour into SkyResults) and RecordSkyResultCopies.
//   - ProbeRelight (decision 3's "Relit probes", Renderer/ProbeRelight.h): when the view shades a relightable irradiance volume
//     and its light changed or its probes were copied again, CSProbeRelight writes each such volume's relit probes into the
//     view's IrradianceProbes copy (t13); then, every frame the view has a reflection probe, each reflection probe's relit
//     intensity into SkyResults, which RecordSkyResultCopies places over its ProbeConstants' Intensity. Recorded only when it
//     runs.
// The view's Prepare binds the specular cube at t3 and writes EnvironmentConstants by MakeSkyEnvironmentConstants
// (Private/LightingInputs.h), so the forward, terrain, SSR, probe and fog shaders do not change. Nothing is spread over frames
// and nothing temporal is added: a frame's lighting is a pure function of that frame (§8.3), regenerated in the frame its key
// changed.
//
// The copy into constant buffers (decision 3, "The copy"). The view's constant buffers are static (isConstantBuffer, initial
// state ConstantBuffer, kept), written from the CPU by RecordViewSetup every frame (writeBuffer: vkCmdUpdateBuffer, so in the
// CopyDest state); the sky's computes write the values that follow the sky into the view's SkyResults buffer (a structured
// buffer of float4, Shared/SkyConstants.h's layout), and RecordSkyResultCopies copies them over the CPU-written bytes with
// copyBuffer, whose automatic barriers move each buffer to CopySource or CopyDest and back to ConstantBuffer before a pass
// binds it. Since writeBuffer and copyBuffer both write in CopyDest, between which NVRHI places no barrier, the copies first
// move their destinations to ConstantBuffer (a barrier completing the CPU writes; synchronization validation reports the
// write-after-write hazard without it). Every Vulkan buffer of the vendored NVRHI is created with TRANSFER_SRC and
// TRANSFER_DST. copyBuffer, unlike writeBuffer, ends an open render pass only when a barrier is pending, so the copies are
// recorded outside every render pass: at the end of the Ambient stage, right after the sky's dispatches. No layout changes:
// the forward, terrain, skybox, SSR, probe and fog shaders read the constants they always read. Proved on the device, under
// validation and synchronization validation at API 1.4 and 1.3, by "SkyLighting: a buffer copied into a view's constant
// buffers after their CPU writes reaches the passes that read them".

namespace Engine {

	class GraphicsDevice;
	class PassBindingCache;

	// The importance samples of the image-based light's prefilter: SkySpecularMinSamples at mip 1 rising linearly to
	// SkySpecularMaxSamples at the last (the bake's 64 to 512 scaled down: the sky is smooth).
	inline constexpr uint32_t SkySpecularMinSamples = 16;
	inline constexpr uint32_t SkySpecularMaxSamples = 64;

	// The atmosphere's key: what the transmittance and multiple-scattering LUTs depend on (MakeSkyAtmosphere's inputs).
	struct SkyLutKey
	{
		float Haze = 0.0f;
		float Overcast = 0.0f;
		glm::vec3 GroundAlbedo = glm::vec3(0.0f);

		bool operator==(const SkyLutKey&) const = default;
	};

	// The image-based light's key (decision 3): the view's SkyConstants with what the update does not read cleared (the
	// camera's altitude and the live cloud offset, since the light is taken at sea level with the clouds at their stepped
	// offset; the fog and Intensity, which the finalize applies every frame; the discs, the stars and Overcast, whose Mie
	// terms stay; and the moon's direction, fraction and flag while no Moon light lights it), and the clouds' offset at
	// SkyCloudLightingStep while they cover anything. Equal keys give the same light.
	struct SkyLightingKey
	{
		SkyConstants Constants{};
		glm::vec2 CloudOffset = glm::vec2(0.0f);

		[[nodiscard]] bool operator==(const SkyLightingKey& other) const; // byte-wise on Constants
	};

	// The view's sky resources (SceneRenderer::State's place-2 members, Architecture §8.14 item 10): created on the first render
	// with a Sky, dropped with the view's state on a device loss and created again by the next render, which then regenerates
	// everything (the keys are empty; a shader reload empties them too). RGBA16F LUTs and cubes; the cubes with their mips.
	struct SkyViewResources
	{
		nvrhi::BufferHandle Constants{};             // SkyConstants, b1
		nvrhi::TextureHandle Transmittance{};        // SkyTransmittanceLutWidth x Height
		nvrhi::TextureHandle MultipleScattering{};   // SkyMultipleScatteringLutSize²
		nvrhi::TextureHandle SkyView{};              // the camera's, every frame
		nvrhi::TextureHandle SeaLevelSkyView{};      // the image-based light's, with each update
		nvrhi::TextureHandle Cube{};                 // SkyCubeSize², its full mip chain
		nvrhi::TextureHandle Specular{};             // SkyCubeSize² x EnvironmentData::SpecularMipCount, bound at t3
		nvrhi::BufferHandle Partials{};              // the SH9 projection's partial sums
		nvrhi::BufferHandle Totals{};                // their reduction: the radiance SH9 and the solid angle
		nvrhi::BufferHandle Results{};               // SkyResults (Shared/SkyConstants.h)
		nvrhi::BufferHandle ReflectionRelights{};    // MaxViewReflectionProbes float4, the reflection probes' relight entries
		std::optional<SkyLutKey> LutKey{};           // the atmosphere the transmittance and multiple-scattering LUTs hold
		std::optional<SkyLightingKey> LightingKey{}; // the light the cubes and totals hold
	};

	// The view's SkyConstants for its enabled `sky`, the snapshot's `lights`, its `camera` and its `fog` (Shared/SkyConstants.h's
	// members): MakeSkyAtmosphere's coefficients, ComputeSkyCameraAltitude, the Sun (toward it from its light, +Y without one;
	// the illuminance it scatters: SunLightColor times its light's Intensity, zero without one; the disc: half its LightAngle
	// and SunDisc over the disc's solid angle, 0 for a LightAngle of 0), the Moon (MoonDirection; half MoonSize; MoonLightColor
	// times its light's Intensity, zero without a Moon light; ComputeMoonIlluminatedFraction), ComputeSkyStarFrame's rows, the
	// clouds (the live offset ComputeSkyCloudOffset with no step), and the fog's Color and AmbientTint made usable as
	// MakeFogConstants makes them. Non-finite or out-of-range values take the component's range or default. Pure; DetMath.
	[[nodiscard]] SkyConstants MakeSkyConstants(const RenderSky& sky, std::span<const LightData> lights, const CameraData& camera, const RenderFog& fog);

	// CloudSpeed (each component clamped to [-100, 100], a non-finite one 0) times `simulationTime` (0 when negative or not
	// finite), floored to a multiple of `step` seconds when `step` is positive, in double, wrapped to the cloud noise's period
	// (1,024 km, Passes/Sky.slang). Pure.
	[[nodiscard]] glm::vec2 ComputeSkyCloudOffset(const glm::vec2& cloudSpeed, double simulationTime, double step);

	// The image-based light's key of a view whose SkyConstants are `constants` (see SkyLightingKey). Pure.
	[[nodiscard]] SkyLightingKey MakeSkyLightingKey(const SkyConstants& constants, const RenderSky& sky);

	// Creates the view's sky resources. Errors: those of GraphicsDevice::CreateBuffer and CreateTexture (Gpu, OutOfMemory),
	// with nothing kept.
	[[nodiscard]] Result<SkyViewResources> CreateSkyViewResources(GraphicsDevice& device);

	// The SkyLighting declaration's pipelines: the Sky program's computes (CSTransmittanceLut, CSMultipleScatteringLut,
	// CSSkyViewLut, CSSkyCube, CSSkyFinalize, CSProbeRelight) and EnvironmentBake's CSDownsampleCube, CSPrefilterSpecular,
	// CSProjectIrradiance and CSReduceIrradiance from EnvironmentBaker::GetLayoutDescriptions, the same SPIR-V. Created once per
	// device at startup and owned by SceneRendererPipelines; binding sets from the view's PassBindingCache. Main thread only; not
	// copyable or movable.
	class SkyLightingPass
	{
	public:
		static constexpr uint32_t PipelineCount = 10;

		class ConstructionKey
		{
			ConstructionKey() = default;
			friend class SkyLightingPass;
		};

		explicit SkyLightingPass(ConstructionKey key);
		~SkyLightingPass();

		SkyLightingPass(const SkyLightingPass&) = delete;
		SkyLightingPass& operator=(const SkyLightingPass&) = delete;

		// Creates the pipelines and the two samplers. `device` is a documented back-reference that outlives the pass. Errors:
		// those of PipelineFactory and the GraphicsDevice wrappers.
		[[nodiscard]] static Result<Scope<SkyLightingPass>> Create(GraphicsDevice& device, PipelineFactory& pipelines);

		// The Sky program's six compute layouts, then the four of the bake's it runs (EnvironmentBaker::GetLayoutDescriptions').
		[[nodiscard]] static std::vector<PipelineLayoutDescription> GetLayoutDescriptions();
		[[nodiscard]] uint32_t GetPipelineCount() const;

		// The SkyLuts row: with `atmosphere`, the transmittance and multiple-scattering LUTs; then the camera's sky-view LUT at
		// `altitude` km. `resources.Constants` holds the view's SkyConstants. Returns the dispatches recorded. Errors: those of
		// creating a binding set (Gpu).
		[[nodiscard]] Result<uint32_t> RecordLuts(nvrhi::ICommandList& commandList, PassBindingCache& bindings, const SkyViewResources& resources, bool atmosphere,
			float altitude);

		// The SkyIbl row's update: the sea-level sky-view LUT, the cube's level 0 with the clouds at `cloudOffset`, its mip chain,
		// the specular prefilter, the radiance SH9's projection and reduction. Returns the dispatches recorded. Errors: as above.
		[[nodiscard]] Result<uint32_t> RecordImageBasedLight(nvrhi::ICommandList& commandList, PassBindingCache& bindings, const SkyViewResources& resources,
			const glm::vec2& cloudOffset);

		// The environment bake's steps on resources.Cube, whose level 0 holds the light (RecordImageBasedLight's last part): the
		// cube's mip chain, the GGX prefilter into resources.Specular and the radiance SH9's projection and reduction into
		// resources.Totals. Returns the dispatches recorded. Errors: as above.
		[[nodiscard]] Result<uint32_t> RecordCubeLighting(nvrhi::ICommandList& commandList, PassBindingCache& bindings, const SkyViewResources& resources);

		// The SkyIbl row's every-frame part: CSSkyFinalize into resources.Results. Errors: as above.
		[[nodiscard]] Status RecordFinalize(nvrhi::ICommandList& commandList, PassBindingCache& bindings, const SkyViewResources& resources);

		// The ProbeRelight row's relight of one volume (W45.3): CSProbeRelight's SkyRelightModeVolume over `probeCount` probes of
		// `relight` (the volume's GPU relight block, SkyRelightProbeVectors float4 a probe) and the sky's totals
		// (resources.Totals, times resources.Constants' Intensity), into `coefficients` (the view's IrradianceProbes copy,
		// UAV-capable) from element `first`. Errors: as above.
		[[nodiscard]] Status RecordProbeRelight(nvrhi::ICommandList& commandList, PassBindingCache& bindings, const SkyViewResources& resources, nvrhi::IBuffer& relight,
			nvrhi::IBuffer& coefficients, uint32_t first, uint32_t probeCount);

		// The ProbeRelight row's reflection scales: CSProbeRelight's SkyRelightModeReflections over the first `reflectionProbes`
		// entries of resources.ReflectionRelights (written by the caller first), reading `coefficients`, into resources.Results'
		// SkyResultsReflectionScales. Errors: as above.
		[[nodiscard]] Status RecordReflectionRelight(nvrhi::ICommandList& commandList, PassBindingCache& bindings, const SkyViewResources& resources,
			nvrhi::IBuffer& coefficients, uint32_t reflectionProbes);
	private:
		[[nodiscard]] Status RecordSkyView(nvrhi::ICommandList& commandList, PassBindingCache& bindings, const SkyViewResources& resources, nvrhi::ITexture& target,
			float altitude);
		[[nodiscard]] Status Dispatch(nvrhi::ICommandList& commandList, PassBindingCache& bindings, const ComputePipeline& pipeline, nvrhi::BindingSetDesc desc,
			const void* pushConstants, uint32_t pushSize, const glm::uvec3& groups);
	private:
		struct State;
	private:
		Scope<State> m_State;
	};

	// Records the copies of `results` (SkyResultsCount float4 elements, Shared/SkyConstants.h) into the view's constant buffers,
	// after the command list's CPU writes of them: the irradiance (SkyResultsIrradiance, 144 bytes) over
	// EnvironmentConstants::IrradianceSH9 of `environment`; the fog's colour (SkyResultsFogColor, 12 bytes) over
	// FogConstants::Color of each of `fogs` (the view's, then each drawn mirror slot's); and, for each i below
	// `reflectionProbes` (at most MaxViewReflectionProbes), SkyResultsReflectionScales + i's x (4 bytes) over
	// ProbeConstants::Reflections[i].AxisScale.w of `probes`; the destinations first move to ConstantBuffer (see the file
	// comment). A null `environment` or `probes` and a null entry of `fogs` are skipped; `results` is never null (asserted).
	// Main thread, outside a render pass.
	void RecordSkyResultCopies(nvrhi::ICommandList& commandList, nvrhi::IBuffer* results, nvrhi::IBuffer* environment, std::span<nvrhi::IBuffer* const> fogs,
		nvrhi::IBuffer* probes, uint32_t reflectionProbes);

}
