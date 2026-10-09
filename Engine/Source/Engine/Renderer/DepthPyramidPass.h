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

	// Pure projection-aware depth reconstruction. Reverse depth in [0,1], valid camera (asserted).
	// d=0 is the background sentinel 65504 for BOTH projections, as the prepass/skybox convention also treats far-plane
	// zero as background. For d>0: perspective near/d; orthographic far-d*(far-near). Clamp to [0,65504] for R16_FLOAT.
	[[nodiscard]] float ReconstructLinearViewDepth(const CameraData& camera, float reverseDepth);
	// View POSITION at positive linear depth z, not a normalized direction times z. uv is top-left [0,1]^2;
	// ndc=(2*u-1,1-2*v). Perspective: (ndc.x*z/P00,ndc.y*z/P11,-z); ortho:
	// (ndc.x*OrthoHalfWidth,ndc.y*OrthoHalfHeight,-z). Valid finite uv/depth/camera asserted.
	[[nodiscard]] glm::vec3 ReconstructViewPosition(const CameraData& camera, const glm::vec2& uv, float linearDepth);

	class GraphicsDevice;

	struct DepthPyramidInputs
	{
		nvrhi::ITexture* SceneDepth = nullptr;
		nvrhi::ITexture* ViewDepth = nullptr; // GetTargetDesc(width,height)
		CameraData Camera{};
	};

	// Shared pipelines, per-view target/cache. Main thread, no temporal state; no borrowed pointer survives Record.
	class DepthPyramidPass
	{
	public:
		static constexpr uint32_t PipelineCount = 2; // projection-aware linearization, conservative minimum reduction
		static constexpr uint32_t MaxMipCount = 5;
		class ConstructionKey
		{
			ConstructionKey() = default;
			friend class DepthPyramidPass;
		};
		explicit DepthPyramidPass(ConstructionKey key);
		~DepthPyramidPass();
		DepthPyramidPass(const DepthPyramidPass&) = delete;
		DepthPyramidPass& operator=(const DepthPyramidPass&) = delete;

		// device back-reference outlives pass; creation errors propagate as in ShadowPass.
		[[nodiscard]] static Result<Scope<DepthPyramidPass>> Create(GraphicsDevice& device, PipelineFactory& pipelines);
		[[nodiscard]] static std::vector<PipelineLayoutDescription> GetLayoutDescriptions();
		// R16_FLOAT, full resolution, min(5, full mip count) mips, ShaderResource/UAV, keepInitialState.
		// width/height >= 1 asserted; mip extents floor by native texture rules. Reduction uses the exact
		// footprint below (2x2 for even dimensions, up to 3x3 for odd); every source texel participates.
		[[nodiscard]] static nvrhi::TextureDesc GetTargetDesc(uint32_t width, uint32_t height);
		// InvalidArgument for invalid camera or mismatched descriptors, Gpu on binding creation; open list asserted.
		[[nodiscard]] Status Record(nvrhi::ICommandList& commandList, RenderRecordingContext& recording, PassBindingCache& bindings, const DepthPyramidInputs& inputs);
		// Record opens/closes DepthPyramid.
		// Borrow recording only for this call; disabled work has no row, errors still close scopes and report recorded counters.
	private:
		struct State;
		Scope<State> m_State;
	};

	struct DepthReductionFootprint
	{
		uint32_t MinX = 0;
		uint32_t MinY = 0;
		uint32_t MaxXExclusive = 0;
		uint32_t MaxYExclusive = 0;
	};

	// Pure CPU oracle contract. Destination extent D=max(1,S/2). Each output coordinate i covers
	// [floor(i*S/D), ceil((i+1)*S/D)), using uint64 intermediates. Fractional-boundary overlap is conservative.
	// Minimum over the entire rectangular footprint; dimensions of one remain one. InvalidArgument for zero
	// source dimensions or output coordinates outside D. CPU expectations must be independent of shader output.
	[[nodiscard]] Result<DepthReductionFootprint> ComputeDepthReductionFootprint(uint32_t sourceWidth, uint32_t sourceHeight,
		uint32_t x, uint32_t y);

}
