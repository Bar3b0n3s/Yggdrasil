#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Graphics/PipelineFactory.h"
#include "Engine/Renderer/PassBindingCache.h"
#include "Engine/Renderer/RenderSnapshot.h"

#include <glm/glm.hpp>
#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <span>
#include <vector>

// The Lights2D pass (M21, C21.2, R-2D-LIGHT; Docs/Decisions/0032-m21-contract.md decision 22): the 2D lights a view draws,
// added into its light accumulation, Lights2DLight (RGBA16_FLOAT at the scene resolution, Renderer/SceneTargetFormats.h),
// which the QuadBatcher's HdrLit pipeline multiplies the lit sprites and tiles by. One pipeline of the Lights2D program
// (Passes/Lights2D.slang, its declaration in RenderPassDeclaration.h) with additive blending (one, one), no depth, no
// culling; one draw per light: a Point light's square around its position in its plane, projected by the view with its
// depth held inside the clip range, and a Global light's triangle over the whole target. Each fragment computes
// Renderer/Light2DShading.h's model, reading the lit items' normals (Lights2DNormals, which the QuadBatcher's Normals
// pipeline wrote) at its pixel.
//
// The renderer (the Sprites stage, SceneRenderer.cpp) decides which lights the view draws: the snapshot's Lights2D that
// reach the view (Light2DReachesView), in their order, at most MaxVisibleLights2D of them. Record adds exactly the lights it
// is given, skipping one whose values are not valid (IsLight2DValid) as a second guard.
//
// Created once per device at startup and owned by SceneRendererPipelines, which creates it through CreatePipelines (place
// 1 of Docs/Decisions/0029-m18-contract.md decision 27); the pass keeps no binding sets: each Record takes its one set (the
// normals) from the view's PassBindingCache (place 2), and the targets come from the view's RenderTargetPool, so the
// recreation protocol covers the pass without hooks of its own. Main thread only; not copyable or movable.

namespace Engine {

	struct RenderPassCounters;

	class GraphicsDevice;

	struct Lights2DPassInputs
	{
		// The lights to add, in their order (the renderer's selection, see the file comment).
		std::span<const Light2DItem> Lights{};
		// The view's projection times its view (reverse-Z clip space, §8.3).
		glm::mat4 ViewProjection = glm::mat4(1.0f);
		// Lights2DNormals: Lights2DNormalsFormat, shader-readable, the size of Light's target; never null.
		nvrhi::ITexture* Normals = nullptr;
		// Lights2DLight's framebuffer (GetLights2DLightFramebufferInfo); never null. Record clears its texture to zero first.
		nvrhi::IFramebuffer* Light = nullptr;
	};

	class Lights2DPass
	{
	public:
		static constexpr uint32_t PipelineCount = 1;

		// Restricts construction to Create; CreateScope still reaches the constructor.
		class ConstructionKey
		{
			ConstructionKey() = default;
			friend class Lights2DPass;
		};

		// Use Create.
		explicit Lights2DPass(ConstructionKey key);
		~Lights2DPass();

		Lights2DPass(const Lights2DPass&) = delete;
		Lights2DPass& operator=(const Lights2DPass&) = delete;

		// Creates the pipeline (checked against its reflection). `device` is a documented back-reference that outlives the
		// pass. Errors: those of PipelineFactory and the GraphicsDevice wrappers (Gpu: an out-of-memory creation, §8.14 item
		// 7).
		[[nodiscard]] static Result<Scope<Lights2DPass>> Create(GraphicsDevice& device, PipelineFactory& pipelines);

		// The layout description of the pipeline, for "Shaders: LayoutsMatchReflection".
		[[nodiscard]] static std::vector<PipelineLayoutDescription> GetLayoutDescriptions();
		[[nodiscard]] uint32_t GetPipelineCount() const;

		// Clears the light accumulation of `inputs` to zero, then adds every valid light of inputs.Lights, one draw each, with
		// its binding set from the view's `bindings`, adding the draws and triangles to `counters`. Errors: InvalidArgument for
		// a null Normals or Light (asserted); Gpu when the binding set cannot be created (nothing is recorded, not even the
		// clear).
		[[nodiscard]] Status Record(nvrhi::ICommandList& commandList, PassBindingCache& bindings, const Lights2DPassInputs& inputs,
			RenderPassCounters& counters);
	private:
		// The back-reference and the pipeline (Lights2DPass.cpp).
		struct State;
	private:
		Scope<State> m_State;
	};

}
