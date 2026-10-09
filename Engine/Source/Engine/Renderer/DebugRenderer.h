#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Graphics/PipelineFactory.h"
#include "Engine/Renderer/DebugDrawList.h"
#include "Engine/Renderer/PassBindingCache.h"

#include <glm/glm.hpp>
#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <vector>

// §8.3 pass 13's debug lines (Architecture §8.10): the RenderSnapshot's DebugDrawList drawn after tonemapping into the final
// LDR target as 1 px LineList (wide lines are unavailable on MoltenVK) from one geometrically growing dynamic vertex
// buffer, with two pipelines: depth-tested against SceneDepth (GreaterOrEqual, no depth writes) and on top (no depth test).
// Colours are linear with straight alpha: the vertex shader encodes them with the sRGB OETF (the target holds
// display-encoded values, §8.9) and the pipelines blend with straight alpha. DebugText commands are labels drawn by
// TextRenderer, not here. Primitives with a non-finite value, or with finite values that tessellate to a vertex beyond
// float's range, are skipped. Deterministic: commands in list order.
//
// Binding layout (set 0): b0 ViewConstants (ViewProjection). The pipelines are created for GetOverlayFramebufferInfo()
// (SceneTargetFormats.h). Created once per device at startup and owned by SceneRendererPipelines; any number of views record
// with it, one at a time (each record writes the vertex buffer through its command list, which NVRHI orders after earlier
// reads); the renderer keeps no binding sets: each Record takes them from the view's PassBindingCache. At most
// MaxDebugLineVertices vertices per record, so a runaway producer cannot exhaust CPU or GPU memory either (the list's
// MaxCommands bounds commands, not their tessellation). Main thread only; not copyable or movable. Frozen by the M8
// contract (Docs/Decisions/0013-m8-decisions.md decision 10).

namespace Engine {

	struct RenderPassCounters;

	class GraphicsDevice;

	// The segments of a full circle, a sphere's great circles and a capsule's rings.
	inline constexpr uint32_t DebugCircleSegments = 32;

	// The most line vertices one record draws, both depth modes together (2^20 vertices of 28 bytes: 28 MiB, the vertex
	// buffer's largest size; 65,536 capsules of 264 vertices each would otherwise take about 484 MB per frame).
	inline constexpr uint32_t MaxDebugLineVertices = 1U << 20;

	// One line vertex: world position and linear, straight-alpha colour.
	struct DebugLineVertex
	{
		glm::vec3 Position = glm::vec3(0.0f);
		glm::vec4 Color = glm::vec4(1.0f);

		bool operator==(const DebugLineVertex&) const = default;
	};

	// The line list of a DebugDrawList, two vertices per segment, by depth mode.
	struct DebugLineVertices
	{
		std::vector<DebugLineVertex> Tested{};
		std::vector<DebugLineVertex> OnTop{};
		// Commands with a non-finite value or a tessellated vertex beyond float's range (DebugText commands are not lines and
		// not counted).
		uint32_t SkippedCommands = 0;
		uint32_t BudgetSkippedCommands = 0; // commands left out because their segments would exceed MaxDebugLineVertices
	};

	// Tessellates `list` into segments, in command order (pure; CPU-testable). Segments per primitive: Line 1; Ray 1 (Origin
	// to Origin + normalize(Direction) * Length; a zero direction is skipped); Box 12 (its edges); Sphere
	// 3 * DebugCircleSegments (great circles in the world XY, YZ and ZX planes); Capsule 2 * DebugCircleSegments (the rings
	// around both hemisphere centres, perpendicular to the axis) + 4 (the sides) + 2 * DebugCircleSegments (the two
	// hemisphere arcs in each of the two planes through the axis, DebugCircleSegments / 2 segments each); Arrow 1 + 4 (the
	// shaft and four barbs of length HeadSize at 30 degrees around the shaft); Frustum 12; Text 0. A command whose segments
	// would take Tested.size() + OnTop.size() past MaxDebugLineVertices is left out whole and counted in
	// BudgetSkippedCommands (a later, smaller command may still fit).
	[[nodiscard]] DebugLineVertices BuildDebugLineVertices(const DebugDrawList& list);

	struct DebugRenderInputs
	{
		const DebugDrawList* DebugDraw = nullptr; // never null
		// The final LDR target with SceneDepth (GetOverlayFramebufferInfo); never null.
		nvrhi::IFramebuffer* Framebuffer = nullptr;
		nvrhi::IBuffer* ViewConstants = nullptr; // b0; never null
	};

	class DebugRenderer
	{
	public:
		// Depth-tested and on top.
		static constexpr uint32_t PipelineCount = 2;

		// Restricts construction to Create; CreateScope still reaches the constructor.
		class ConstructionKey
		{
			ConstructionKey() = default;
			friend class DebugRenderer;
		};

		// Use Create.
		explicit DebugRenderer(ConstructionKey key);
		~DebugRenderer();

		DebugRenderer(const DebugRenderer&) = delete;
		DebugRenderer& operator=(const DebugRenderer&) = delete;

		// Creates the two pipelines. `device` is a documented back-reference that outlives the renderer. Errors: those of
		// PipelineFactory and the GraphicsDevice wrappers (Gpu: FatalError(OutOfMemory) at startup, §8.14 item 7).
		[[nodiscard]] static Result<Scope<DebugRenderer>> Create(GraphicsDevice& device, PipelineFactory& pipelines);

		[[nodiscard]] static std::vector<PipelineLayoutDescription> GetLayoutDescriptions();
		[[nodiscard]] uint32_t GetPipelineCount() const;

		// Records the list's lines (BuildDebugLineVertices) into `inputs.Framebuffer`, with binding sets from the view's
		// `bindings`, growing the vertex buffer geometrically (up to MaxDebugLineVertices) when it is too small. Logs a
		// Warning once per DebugRenderer the first time commands are left out for the vertex budget. Returns the number of
		// vertices drawn. Asserts the inputs. Errors: Gpu when the vertex buffer or a binding set cannot be created (the
		// lines are skipped).
		[[nodiscard]] Result<uint32_t> Record(nvrhi::ICommandList& commandList, PassBindingCache& bindings, const DebugRenderInputs& inputs);
	private:
		friend class SceneRenderer;
		[[nodiscard]] Result<uint32_t> RecordCounted(nvrhi::ICommandList& commandList, PassBindingCache& bindings, const DebugRenderInputs& inputs, RenderPassCounters& counters);
		// The back-reference, the pipelines and the vertex buffer (DebugRenderer.cpp).
		struct State;
	private:
		Scope<State> m_State;
	};

}
