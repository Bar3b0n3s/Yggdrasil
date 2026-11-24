#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Graphics/PipelineFactory.h"
#include "Engine/Renderer/PassBindingCache.h"

#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <optional>
#include <vector>

// M45's Sky pass (Docs/Decisions/0063-m45-sky-and-time-of-day.md decision 3; Architecture §8.3 "Sky"): the procedural sky,
// drawn in the Skybox's place by a Lit view whose camera clears to the skybox and whose snapshot's Sky is enabled. One
// fullscreen triangle at depth 0 into SceneColor tested against SceneDepth (GreaterOrEqual, writes off), as SkyboxPass draws:
// each pixel adds the view's sky-view LUT (Renderer/SkyLighting.h), the clouds at the live offset, the stars, the moon and the
// sun's disc, times the Sky's Intensity, fogged with FogSkybox as the skybox is (Passes/Sky.slang's PSMain). Mirror views draw
// it with their scissor and fog start, sampling the main view's sky-view LUT (world directions); a probe capture draws it from
// the probe's altitude.
//
// Binding layout (set 0): b0 ViewConstants, b1 SkyConstants, b5 FogConstants, t0 the transmittance LUT, t2 the sky-view LUT,
// s0 LinearClamp, s1 LinearWrapU (the sky-view LUT wraps in azimuth), and the program's push constants (unread). The pipeline is created for
// GetSceneColorFramebufferInfo() (SceneTargetFormats.h), once per device at startup, owned by SceneRendererPipelines; binding
// sets come from the view's PassBindingCache. Main thread only; not copyable or movable.

namespace Engine {

	class GraphicsDevice;

	struct SkyPassInputs
	{
		nvrhi::IFramebuffer* Framebuffer = nullptr; // SceneColor with SceneDepth; never null
		nvrhi::IBuffer* ViewConstants = nullptr;    // b0; never null
		nvrhi::IBuffer* SkyConstants = nullptr;     // b1; never null
		nvrhi::IBuffer* FogConstants = nullptr;     // b5; never null
		nvrhi::ITexture* Transmittance = nullptr;   // t0; never null
		nvrhi::ITexture* SkyView = nullptr;         // t2; never null
		std::optional<nvrhi::Rect> Scissor{};       // a mirror view's glass rectangle; the whole framebuffer when empty
	};

	class SkyPass
	{
	public:
		static constexpr uint32_t PipelineCount = 1;

		class ConstructionKey
		{
			ConstructionKey() = default;
			friend class SkyPass;
		};

		explicit SkyPass(ConstructionKey key);
		~SkyPass();

		SkyPass(const SkyPass&) = delete;
		SkyPass& operator=(const SkyPass&) = delete;

		// Creates the pipeline and the two samplers. `device` is a documented back-reference that outlives the pass. Errors:
		// those of PipelineFactory and the GraphicsDevice wrappers.
		[[nodiscard]] static Result<Scope<SkyPass>> Create(GraphicsDevice& device, PipelineFactory& pipelines);

		[[nodiscard]] static std::vector<PipelineLayoutDescription> GetLayoutDescriptions();
		[[nodiscard]] uint32_t GetPipelineCount() const;

		// Records the sky into `inputs.Framebuffer`. Asserts the inputs. Errors: those of creating a binding set (Gpu).
		[[nodiscard]] Status Record(nvrhi::ICommandList& commandList, PassBindingCache& bindings, const SkyPassInputs& inputs);
	private:
		struct State;
	private:
		Scope<State> m_State;
	};

}
