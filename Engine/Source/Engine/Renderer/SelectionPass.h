#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Graphics/PipelineFactory.h"
#include "Engine/Renderer/PassBindingCache.h"
#include "Engine/Renderer/RenderSnapshot.h"
#include "Engine/Renderer/RenderStats.h"

#include <nvrhi/nvrhi.h>

#include <span>
#include <vector>

namespace Engine {

	class AssetManager;
	class GpuResourceCache;
	class GraphicsDevice;

	struct SelectionRenderInputs
	{
		const RenderSnapshot* Snapshot = nullptr; // borrowed only for Record
		nvrhi::ITexture* SceneDepth = nullptr;
		nvrhi::ITexture* Mask = nullptr;                      // R8_UNORM, full view extent; 1 visible, 0.5 occluded
		nvrhi::ITexture* Scratch = nullptr;                   // R8_UNORM dilation intermediate, distinct from Mask
		nvrhi::ITexture* LdrColor = nullptr;                  // RGBA8_UNORM composite target
		uint32_t Radius = 2;                                  // framebuffer pixels, [1,8]
		glm::vec4 Color = glm::vec4(1.0f, 0.55f, 0.1f, 1.0f); // linear/straight alpha; encode before composite
	};

	// Shared immutable pipelines, per-view targets/cache; main thread. Mask preserves M8 alpha/cull parity for Opaque
	// and Mask materials; selected Blend geometry uses geometric coverage. No stencil. Occluded edges dim to half alpha.
	class SelectionPass
	{
	public:
		static constexpr uint32_t PipelineCount = 8; // 6 mask variants + separable dilation + edge composite
		class ConstructionKey
		{
			ConstructionKey() = default;
			friend class SelectionPass;
		};
		explicit SelectionPass(ConstructionKey key);
		~SelectionPass();
		SelectionPass(const SelectionPass&) = delete;
		SelectionPass& operator=(const SelectionPass&) = delete;
		[[nodiscard]] static Result<Scope<SelectionPass>> Create(GraphicsDevice& device, PipelineFactory& pipelines);
		[[nodiscard]] static std::vector<PipelineLayoutDescription> GetLayoutDescriptions();
		[[nodiscard]] static nvrhi::TextureDesc GetMaskDesc(uint32_t width, uint32_t height);
		// Empty selection clears mask and does no composite; invalid/deleted ids are ignored. InvalidArgument for invalid
		// radius/descriptors; Gpu from resources/bindings. commandList open asserted; all inputs borrowed for this call.
		[[nodiscard]] Status Record(nvrhi::ICommandList& commandList, RenderRecordingContext& recording, PassBindingCache& bindings, GpuResourceCache& cache,
			AssetManager& assets, const SelectionRenderInputs& inputs);
		// Record opens/closes SelectionMask, SelectionDilateHorizontal, SelectionDilateVertical, SelectionComposite.
		// Borrow recording only for this call; disabled work has no row, errors still close scopes and report recorded counters.
	private:
		struct State;
		Scope<State> m_State;
	};

}
