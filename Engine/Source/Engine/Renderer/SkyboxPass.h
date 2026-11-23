#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Graphics/PipelineFactory.h"
#include "Engine/Renderer/PassBindingCache.h"

#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <optional>
#include <vector>

// §8.3 pass 8, the skybox (Architecture §8.6): one fullscreen triangle at depth 0 drawn into SceneColor tested against
// SceneDepth (depth test GreaterOrEqual, writes off), so it covers exactly the pixels the scene left at the cleared depth 0.
// Each pixel samples the environment's skybox cube (EnvironmentData::Skybox, RGBA16_FLOAT with mips) along its view ray
// (ViewConstants: per pixel from InverseViewProjection for perspective cameras; the constant view direction for
// orthographic ones, §8.3), rotated by the environment's Rotation, at the mip level SkyboxLod (SkyboxBlur scaled to the
// cube's mip chain), times Intensity, all from EnvironmentConstants (Shared/EnvironmentConstants.h). Linear radiance out:
// exposure and tonemapping follow in TonemapPass.
//
// M32 (Docs/Decisions/0047-m32-fog-ssr.md decision 5): in a fogged view whose fog has FogSkybox, each pixel is fogged as the
// sky at infinite depth along its view ray (Common/Fog.slang's EvaluateSkyFog, Renderer/Fog.h); without fog it is unchanged.
// M32's milestone review: with FogConstants::SkyIsClearColor the pass draws the fog's ClearColor in the cube's place (the
// renderer records it so in a fogged view with FogSkybox whose cleared pixels no skybox draws over, binding a black cube).
//
// Binding layout (set 0, the registers of §8.4): b0 ViewConstants, b2 EnvironmentConstants, b5 FogConstants (M32), t4
// EnvSkybox, s0 LinearClamp.
// The pipeline is created for GetSceneColorFramebufferInfo() (SceneTargetFormats.h). Created once per device at startup and
// owned by SceneRendererPipelines; the pass keeps no binding sets: each Record takes them from the view's PassBindingCache.
// Main thread only; not copyable or movable. Frozen by the M8 contract (Docs/Decisions/0013-m8-decisions.md decision 9).

namespace Engine {

	class GraphicsDevice;

	struct SkyboxPassInputs
	{
		// SceneColor with SceneDepth (GetSceneColorFramebufferInfo); the forward pass's framebuffer. Never null.
		nvrhi::IFramebuffer* Framebuffer = nullptr;
		nvrhi::IBuffer* ViewConstants = nullptr;        // b0; never null
		nvrhi::IBuffer* EnvironmentConstants = nullptr; // b2; never null
		nvrhi::IBuffer* FogConstants = nullptr;         // b5 (M32: Shared/FogConstants.h); never null
		nvrhi::ITexture* Skybox = nullptr;              // the environment's skybox cube (GpuEnvironment::Skybox); never null
		// M42 (Docs/Decisions/0059-m42-planar-reflections.md decision 3): the rectangle rasterization is limited to (a mirror
		// view's glass rectangle, widened by its blur's footprint); the whole framebuffer when empty.
		std::optional<nvrhi::Rect> Scissor{};
	};

	class SkyboxPass
	{
	public:
		static constexpr uint32_t PipelineCount = 1;

		// Restricts construction to Create; CreateScope still reaches the constructor.
		class ConstructionKey
		{
			ConstructionKey() = default;
			friend class SkyboxPass;
		};

		// Use Create.
		explicit SkyboxPass(ConstructionKey key);
		~SkyboxPass();

		SkyboxPass(const SkyboxPass&) = delete;
		SkyboxPass& operator=(const SkyboxPass&) = delete;

		// Creates the pipeline and the LinearClamp sampler. `device` is a documented back-reference that outlives the pass.
		// Errors: those of PipelineFactory and the GraphicsDevice wrappers (Gpu: FatalError(OutOfMemory) at startup, §8.14
		// item 7).
		[[nodiscard]] static Result<Scope<SkyboxPass>> Create(GraphicsDevice& device, PipelineFactory& pipelines);

		[[nodiscard]] static std::vector<PipelineLayoutDescription> GetLayoutDescriptions();
		[[nodiscard]] uint32_t GetPipelineCount() const;

		// Records the skybox into `inputs.Framebuffer` over its whole extent, with binding sets from the view's `bindings`.
		// Asserts the inputs. Errors: those of creating a binding set for new resources (Gpu).
		[[nodiscard]] Status Record(nvrhi::ICommandList& commandList, PassBindingCache& bindings, const SkyboxPassInputs& inputs);
	private:
		// The back-reference, the pipeline and the sampler (SkyboxPass.cpp).
		struct State;
	private:
		Scope<State> m_State;
	};

}
