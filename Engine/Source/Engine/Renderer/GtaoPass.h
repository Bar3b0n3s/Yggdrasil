#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Graphics/PipelineFactory.h"
#include "Engine/Renderer/PassBindingCache.h"
#include "Engine/Renderer/RenderSnapshot.h"
#include "Engine/Renderer/RenderStats.h"

#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <vector>

namespace Engine {

	class GraphicsDevice;

	// Pure §8.8 radius in pixels: radius*.5*height*abs(P[1][1])/linearDepth (perspective),
	// radius*.5*height/OrthographicSize (orthographic). InvalidArgument for invalid camera/radius/depth.
	[[nodiscard]] Result<float> ComputeGtaoScreenRadius(const CameraData& camera, float radius, float linearDepth);
	[[nodiscard]] uint32_t GetGtaoSliceCount(RenderSsaoQuality quality); // Low 1, Medium 2, High 3; enum asserted

	struct GtaoInputs
	{
		CameraData Camera{};
		PostProcessSettings Post{};
		bool HalfResolution = false; // RenderingSettings.SsaoHalfResolution OR Low quality
		nvrhi::ITexture* ViewDepth = nullptr;
		nvrhi::ITexture* SceneNormals = nullptr; // RG16_FLOAT octahedral normal-mapped view normals
		nvrhi::ITexture* Occlusion = nullptr;    // R8_UNORM, output of second denoise
		nvrhi::ITexture* Scratch = nullptr;      // same descriptor, ping-pong; never aliases Occlusion
	};

	// In-house GTAO: 1/2/3 slices, 3 steps per side, horizon integration/thickness heuristic, fixed IGN rotation,
	// two depth+normal-aware bilateral denoise passes. No temporal jitter or reprojection.
	// Shared pipelines; targets/cache owned per view; all raw inputs borrowed only for Record. Main thread.
	class GtaoPass
	{
	public:
		static constexpr uint32_t PipelineCount = 2; // main and bilateral denoise (axis constant)
		class ConstructionKey
		{
			ConstructionKey() = default;
			friend class GtaoPass;
		};
		explicit GtaoPass(ConstructionKey key);
		~GtaoPass();
		GtaoPass(const GtaoPass&) = delete;
		GtaoPass& operator=(const GtaoPass&) = delete;
		[[nodiscard]] static Result<Scope<GtaoPass>> Create(GraphicsDevice& device, PipelineFactory& pipelines);
		[[nodiscard]] static std::vector<PipelineLayoutDescription> GetLayoutDescriptions();
		// width,height >=1 asserted; half uses ceil(width/2),ceil(height/2), at least 1.
		[[nodiscard]] static nvrhi::TextureDesc GetTargetDesc(uint32_t width, uint32_t height, bool halfResolution);
		// Disabled SSAO clears AO to white; otherwise returns fully denoised Occlusion. InvalidArgument for invalid
		// camera/post values or incompatible textures; Gpu on bindings. Caller uses bilateral upsample when half-sized.
		// Forward applies min(GTAO,materialAO) to indirect diffuse/specular only, with multi-bounce diffuse correction.
		[[nodiscard]] Result<nvrhi::ITexture*> Record(nvrhi::ICommandList& commandList, RenderRecordingContext& recording, PassBindingCache& bindings, const GtaoInputs& inputs);
		// Record opens/closes GTAO, GTAODenoiseHorizontal, GTAODenoiseVertical.
		// Borrow recording only for this call; disabled work has no row, errors still close scopes and report recorded counters.
	private:
		struct State;
		Scope<State> m_State;
	};

}
