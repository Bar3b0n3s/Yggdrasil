#include "EnginePCH.h"
#include "Engine/Renderer/DebugRenderer.h"

#include "Engine/Core/Assert.h"
#include "Engine/Core/Log.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Renderer/SceneTargetFormats.h"
#include "Shared/ViewConstants.h"

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

// The tessellation writes each primitive's segments straight into the vertex list of its depth mode, after checking the
// primitive's values and the vertex budget, so a skipped command leaves nothing behind. The vertex buffer holds the
// depth-tested vertices followed by the on-top ones and is drawn with two draws, one per pipeline.

namespace Engine {

	namespace Utils {

		constexpr std::string_view DebugLinesProgram = "DebugLines";
		// The first capacity of the vertex buffer, in vertices; it doubles until a record fits, up to MaxDebugLineVertices.
		constexpr uint32_t InitialDebugLineVertexCapacity = 4096;
		// Indices of the two pipelines (GetLayoutDescriptions' order).
		constexpr size_t DebugLinesTestedPipeline = 0;
		constexpr size_t DebugLinesOnTopPipeline = 1;
		// An arrowhead's barbs leave the shaft's tip at this angle (degrees).
		constexpr double ArrowBarbAngleDegrees = 30.0;

		static_assert(sizeof(DebugLineVertex) == 28, "a debug line vertex is a float3 position and a float4 colour (MaxDebugLineVertices)");
		static_assert(DebugCircleSegments % 2 == 0, "a capsule's hemisphere arcs take half a circle each");

		// The segments each primitive tessellates into (DebugRenderer.h).
		struct DebugSegmentCounts
		{
			static constexpr uint32_t Line = 1;
			static constexpr uint32_t Ray = 1;
			static constexpr uint32_t Box = 12;
			static constexpr uint32_t Sphere = 3 * DebugCircleSegments;
			static constexpr uint32_t Capsule = 2 * DebugCircleSegments + 4 + 2 * DebugCircleSegments;
			static constexpr uint32_t Arrow = 1 + 4;
			static constexpr uint32_t Frustum = 12;
		};

		// (cos, sin) of 2 pi k / DebugCircleSegments for k in [0, DebugCircleSegments]; the last point equals the first, so a
		// circle closes exactly.
		static const std::array<glm::vec2, DebugCircleSegments + 1>& GetUnitCircle()
		{
			static const std::array<glm::vec2, DebugCircleSegments + 1> UnitCircle = []()
			{
				std::array<glm::vec2, DebugCircleSegments + 1> points{};
				for (uint32_t index = 0; index < DebugCircleSegments; ++index)
				{
					const double angle = 2.0 * std::numbers::pi * static_cast<double>(index) / static_cast<double>(DebugCircleSegments);
					points[index] = glm::vec2(static_cast<float>(std::cos(angle)), static_cast<float>(std::sin(angle)));
				}
				points[DebugCircleSegments] = points[0];
				return points;
			}();
			return UnitCircle;
		}

		static bool IsFinite(float value)
		{
			return std::isfinite(value);
		}

		template<glm::length_t Length>
		static bool IsFinite(const glm::vec<Length, float>& vector)
		{
			for (glm::length_t component = 0; component < Length; ++component)
			{
				if (!std::isfinite(vector[component]))
					return false;
			}
			return true;
		}

		static bool IsFinite(const glm::quat& rotation)
		{
			return IsFinite(glm::vec4(rotation.x, rotation.y, rotation.z, rotation.w));
		}

		static bool IsFinite(const DebugLine& line)
		{
			return IsFinite(line.From) && IsFinite(line.To);
		}

		static bool IsFinite(const DebugRay& ray)
		{
			return IsFinite(ray.Origin) && IsFinite(ray.Direction) && IsFinite(ray.Length);
		}

		static bool IsFinite(const DebugBox& box)
		{
			return IsFinite(box.Center) && IsFinite(box.HalfExtents) && IsFinite(box.Rotation);
		}

		static bool IsFinite(const DebugSphere& sphere)
		{
			return IsFinite(sphere.Center) && IsFinite(sphere.Radius);
		}

		static bool IsFinite(const DebugCapsule& capsule)
		{
			return IsFinite(capsule.Start) && IsFinite(capsule.End) && IsFinite(capsule.Radius);
		}

		static bool IsFinite(const DebugArrow& arrow)
		{
			return IsFinite(arrow.From) && IsFinite(arrow.To) && IsFinite(arrow.HeadSize);
		}

		static bool IsFinite(const DebugFrustum& frustum)
		{
			return std::ranges::all_of(frustum.Corners, [](const glm::vec3& corner)
			{
				return IsFinite(corner);
			});
		}

		// Two unit vectors perpendicular to the unit vector `axis` and to each other, (axis, first, second) right-handed.
		static std::pair<glm::vec3, glm::vec3> MakePerpendicularBasis(const glm::vec3& axis)
		{
			const glm::vec3 helper = std::abs(axis.y) < 0.99f ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
			const glm::vec3 first = glm::normalize(glm::cross(helper, axis));
			return { first, glm::cross(axis, first) };
		}

		// Appends the segments of one primitive to a vertex list, all with one colour.
		class SegmentWriter
		{
		public:
			SegmentWriter(std::vector<DebugLineVertex>& vertices, const glm::vec4& color)
				: m_Vertices(vertices), m_Color(color)
			{
			}

			void Add(const glm::vec3& from, const glm::vec3& to)
			{
				m_Vertices.push_back({ .Position = from, .Color = m_Color });
				m_Vertices.push_back({ .Position = to, .Color = m_Color });
			}

			// The circle around `center` of radius `radius` in the plane of the unit vectors `u` and `v`, as DebugCircleSegments
			// segments starting at center + radius * u.
			void AddCircle(const glm::vec3& center, float radius, const glm::vec3& u, const glm::vec3& v)
			{
				AddArc(center, radius, u, v, DebugCircleSegments);
			}

			// The first `segments` segments of that circle, from center + radius * u towards center + radius * v.
			void AddArc(const glm::vec3& center, float radius, const glm::vec3& u, const glm::vec3& v, uint32_t segments)
			{
				const std::array<glm::vec2, DebugCircleSegments + 1>& circle = GetUnitCircle();
				for (uint32_t index = 0; index < segments; ++index)
				{
					const glm::vec3 from = center + radius * (circle[index].x * u + circle[index].y * v);
					const glm::vec3 to = center + radius * (circle[index + 1].x * u + circle[index + 1].y * v);
					Add(from, to);
				}
			}
		private:
			std::vector<DebugLineVertex>& m_Vertices;
			glm::vec4 m_Color;
		};

		// The segments a command adds, or 0 when it draws no lines (DebugText).
		struct SegmentCountVisitor
		{
			uint32_t operator()(const DebugLine& /*line*/) const { return DebugSegmentCounts::Line; }
			uint32_t operator()(const DebugRay& /*ray*/) const { return DebugSegmentCounts::Ray; }
			uint32_t operator()(const DebugBox& /*box*/) const { return DebugSegmentCounts::Box; }
			uint32_t operator()(const DebugSphere& /*sphere*/) const { return DebugSegmentCounts::Sphere; }
			uint32_t operator()(const DebugCapsule& /*capsule*/) const { return DebugSegmentCounts::Capsule; }
			uint32_t operator()(const DebugArrow& /*arrow*/) const { return DebugSegmentCounts::Arrow; }
			uint32_t operator()(const DebugFrustum& /*frustum*/) const { return DebugSegmentCounts::Frustum; }
			uint32_t operator()(const DebugText& /*text*/) const { return 0; }
		};

		// Whether a command can be drawn: every value finite, and a ray's direction not zero.
		struct DrawableVisitor
		{
			bool operator()(const DebugLine& line) const { return IsFinite(line); }

			bool operator()(const DebugRay& ray) const
			{
				if (!IsFinite(ray))
					return false;
				const float length = glm::length(ray.Direction);
				return length > 0.0f && std::isfinite(length);
			}

			bool operator()(const DebugBox& box) const { return IsFinite(box); }
			bool operator()(const DebugSphere& sphere) const { return IsFinite(sphere); }
			bool operator()(const DebugCapsule& capsule) const { return IsFinite(capsule); }
			bool operator()(const DebugArrow& arrow) const { return IsFinite(arrow); }
			bool operator()(const DebugFrustum& frustum) const { return IsFinite(frustum); }
			bool operator()(const DebugText& /*text*/) const { return true; }
		};

		// Writes a command's segments (DebugRenderer.h's counts); the command was checked by DrawableVisitor.
		struct TessellationVisitor
		{
			SegmentWriter& Writer;

			void operator()(const DebugLine& line) const { Writer.Add(line.From, line.To); }

			void operator()(const DebugRay& ray) const
			{
				const glm::vec3 direction = ray.Direction / glm::length(ray.Direction);
				Writer.Add(ray.Origin, ray.Origin + direction * ray.Length);
			}

			void operator()(const DebugBox& box) const
			{
				const glm::mat3 rotation = glm::mat3_cast(box.Rotation);
				const glm::vec3 x = rotation[0] * box.HalfExtents.x;
				const glm::vec3 y = rotation[1] * box.HalfExtents.y;
				const glm::vec3 z = rotation[2] * box.HalfExtents.z;
				// Corner i has the signs (bit 0: x, bit 1: y, bit 2: z), set for +.
				std::array<glm::vec3, 8> corners{};
				for (uint32_t index = 0; index < corners.size(); ++index)
				{
					corners[index] = box.Center + ((index & 1U) != 0 ? x : -x) + ((index & 2U) != 0 ? y : -y) + ((index & 4U) != 0 ? z : -z);
				}
				// The edges along x, then y, then z: the corner pairs that differ in exactly that bit.
				for (const uint32_t bit : { 1U, 2U, 4U })
				{
					for (uint32_t index = 0; index < corners.size(); ++index)
					{
						if ((index & bit) == 0)
							Writer.Add(corners[index], corners[index | bit]);
					}
				}
			}

			void operator()(const DebugSphere& sphere) const
			{
				const glm::vec3 x(1.0f, 0.0f, 0.0f);
				const glm::vec3 y(0.0f, 1.0f, 0.0f);
				const glm::vec3 z(0.0f, 0.0f, 1.0f);
				Writer.AddCircle(sphere.Center, sphere.Radius, x, y);
				Writer.AddCircle(sphere.Center, sphere.Radius, y, z);
				Writer.AddCircle(sphere.Center, sphere.Radius, z, x);
			}

			void operator()(const DebugCapsule& capsule) const
			{
				// Equal end points have no axis: +Y stands in, and the capsule draws as a sphere.
				const glm::vec3 segment = capsule.End - capsule.Start;
				const float length = glm::length(segment);
				const glm::vec3 axis = length > 0.0f && std::isfinite(length) ? segment / length : glm::vec3(0.0f, 1.0f, 0.0f);
				const auto [u, v] = MakePerpendicularBasis(axis);
				const float radius = capsule.Radius;
				Writer.AddCircle(capsule.Start, radius, u, v);
				Writer.AddCircle(capsule.End, radius, u, v);
				for (const glm::vec3& side : { u, v, -u, -v })
					Writer.Add(capsule.Start + radius * side, capsule.End + radius * side);
				// The hemispheres: in each of the planes (axis, u) and (axis, v), a half circle over End from +side through
				// +axis to -side, and one under Start from +side through -axis to -side.
				for (const glm::vec3& side : { u, v })
				{
					Writer.AddArc(capsule.End, radius, side, axis, DebugCircleSegments / 2);
					Writer.AddArc(capsule.Start, radius, side, -axis, DebugCircleSegments / 2);
				}
			}

			void operator()(const DebugArrow& arrow) const
			{
				Writer.Add(arrow.From, arrow.To);
				// A zero-length arrow has no direction: its barbs collapse onto the tip (zero-length segments draw nothing).
				const glm::vec3 shaft = arrow.To - arrow.From;
				const float length = glm::length(shaft);
				if (!(length > 0.0f) || !std::isfinite(length))
				{
					for (uint32_t barb = 0; barb < 4; ++barb)
						Writer.Add(arrow.To, arrow.To);
					return;
				}
				const glm::vec3 direction = shaft / length;
				const auto [u, v] = MakePerpendicularBasis(direction);
				const float angle = static_cast<float>(ArrowBarbAngleDegrees * std::numbers::pi / 180.0);
				const float back = arrow.HeadSize * std::cos(angle);
				const float out = arrow.HeadSize * std::sin(angle);
				for (const glm::vec3& side : { u, v, -u, -v })
					Writer.Add(arrow.To, arrow.To - direction * back + side * out);
			}

			void operator()(const DebugFrustum& frustum) const
			{
				const std::array<glm::vec3, 8>& corners = frustum.Corners;
				for (size_t index = 0; index < 4; ++index)
				{
					const size_t next = (index + 1) % 4;
					Writer.Add(corners[index], corners[next]);         // the near rectangle
					Writer.Add(corners[index + 4], corners[next + 4]); // the far rectangle
					Writer.Add(corners[index], corners[index + 4]);    // the side edges
				}
			}

			void operator()(const DebugText& /*text*/) const {}
		};

		static PipelineLayoutDescription MakeDebugLinesDescription(std::string name)
		{
			nvrhi::BindingLayoutDesc layout;
			layout.visibility = nvrhi::ShaderType::All;
			layout.registerSpace = 0;
			layout.registerSpaceIsDescriptorSet = true;
			layout.bindings = { nvrhi::BindingLayoutItem::ConstantBuffer(0) };
			return {
				.Name = std::move(name),
				.Program = std::string(DebugLinesProgram),
				.Entries = { "VSMain", "PSMain" },
				.BindingLayouts = { layout },
				.ConstantBuffers = { { .Set = 0, .Register = 0, .ByteSize = sizeof(ViewConstants) } },
			};
		}

		static std::vector<PipelineLayoutDescription> MakeDebugLinesDescriptions()
		{
			return { MakeDebugLinesDescription("DebugLinesTested"), MakeDebugLinesDescription("DebugLinesOnTop") };
		}

		static std::vector<nvrhi::VertexAttributeDesc> MakeDebugLineVertexAttributes()
		{
			constexpr uint32_t Stride = sizeof(DebugLineVertex);
			return {
				nvrhi::VertexAttributeDesc()
					.setName("POSITION")
					.setFormat(nvrhi::Format::RGB32_FLOAT)
					.setOffset(static_cast<uint32_t>(offsetof(DebugLineVertex, Position)))
					.setElementStride(Stride),
				nvrhi::VertexAttributeDesc()
					.setName("COLOR")
					.setFormat(nvrhi::Format::RGBA32_FLOAT)
					.setOffset(static_cast<uint32_t>(offsetof(DebugLineVertex, Color)))
					.setElementStride(Stride),
			};
		}

		// A debug-line pipeline: 1 px lines with straight alpha into the overlay framebuffer, depth-tested against SceneDepth
		// (reverse-Z, GreaterOrEqual, no writes) or on top.
		static GraphicsPipelineSpecification MakeDebugLinesSpecification(PipelineLayoutDescription layout, bool depthTested,
			const std::vector<nvrhi::BindingLayoutHandle>& sharedLayouts)
		{
			GraphicsPipelineSpecification specification;
			specification.Layout = std::move(layout);
			specification.SharedBindingLayouts = sharedLayouts;
			specification.VertexAttributes = MakeDebugLineVertexAttributes();
			specification.Primitive = nvrhi::PrimitiveType::LineList;
			nvrhi::BlendState::RenderTarget& blend = specification.RenderState.blendState.targets[0];
			blend.enableBlend()
				.setSrcBlend(nvrhi::BlendFactor::SrcAlpha)
				.setDestBlend(nvrhi::BlendFactor::InvSrcAlpha)
				.setBlendOp(nvrhi::BlendOp::Add)
				.setSrcBlendAlpha(nvrhi::BlendFactor::One)
				.setDestBlendAlpha(nvrhi::BlendFactor::InvSrcAlpha)
				.setBlendOpAlpha(nvrhi::BlendOp::Add);
			specification.RenderState.rasterState.setCullNone().enableDepthClip();
			specification.RenderState.depthStencilState.setDepthTestEnable(depthTested)
				.setDepthWriteEnable(false)
				.setDepthFunc(nvrhi::ComparisonFunc::GreaterOrEqual)
				.setStencilEnable(false);
			specification.Framebuffer = GetOverlayFramebufferInfo();
			return specification;
		}

		// The smallest capacity at least `required` reached by doubling from `current` (or from the initial capacity),
		// capped at MaxDebugLineVertices (`required` never exceeds it).
		static uint32_t GrowDebugLineCapacity(uint32_t current, uint32_t required)
		{
			uint32_t capacity = std::max(current, InitialDebugLineVertexCapacity);
			while (capacity < required)
				capacity *= 2;
			return std::min(capacity, MaxDebugLineVertices);
		}

	}

	struct DebugRenderer::State
	{
		GraphicsDevice* Device = nullptr; // documented back-reference
		std::vector<GraphicsPipeline> Pipelines{};
		nvrhi::BufferHandle Vertices{};
		uint32_t VertexCapacity = 0;
		bool HasWarnedBudget = false; // the vertex-budget warning is logged once per renderer
	};

	DebugLineVertices BuildDebugLineVertices(const DebugDrawList& list)
	{
		DebugLineVertices result;
		const std::span<const DebugDrawCommand> commands = list.GetCommands();

		// Reserve what the commands that fit will take, so the lists grow at most once.
		size_t testedReserve = 0;
		size_t onTopReserve = 0;
		for (const DebugDrawCommand& command : commands)
		{
			const size_t vertices = 2 * static_cast<size_t>(std::visit(Utils::SegmentCountVisitor{}, command.Shape));
			if (testedReserve + onTopReserve + vertices > MaxDebugLineVertices)
				continue;
			(command.Depth == DebugDepthMode::OnTop ? onTopReserve : testedReserve) += vertices;
		}
		result.Tested.reserve(testedReserve);
		result.OnTop.reserve(onTopReserve);

		for (const DebugDrawCommand& command : commands)
		{
			if (std::holds_alternative<DebugText>(command.Shape))
				continue; // a label, drawn by the TextRenderer
			if (!Utils::IsFinite(command.Color) || !std::visit(Utils::DrawableVisitor{}, command.Shape))
			{
				++result.SkippedCommands;
				continue;
			}
			const size_t vertices = 2 * static_cast<size_t>(std::visit(Utils::SegmentCountVisitor{}, command.Shape));
			if (result.Tested.size() + result.OnTop.size() + vertices > MaxDebugLineVertices)
			{
				++result.BudgetSkippedCommands;
				continue;
			}
			std::vector<DebugLineVertex>& target = command.Depth == DebugDepthMode::OnTop ? result.OnTop : result.Tested;
			const size_t firstVertex = target.size();
			Utils::SegmentWriter writer(target, command.Color);
			std::visit(Utils::TessellationVisitor{ .Writer = writer }, command.Shape);
			// Finite but huge values can still tessellate beyond float's range (a far, long ray; a huge sphere): such a
			// command is skipped whole, so no non-finite vertex reaches the GPU.
			const bool finite = std::all_of(target.begin() + static_cast<std::ptrdiff_t>(firstVertex), target.end(), [](const DebugLineVertex& vertex)
			{
				return Utils::IsFinite(vertex.Position);
			});
			if (!finite)
			{
				target.resize(firstVertex);
				++result.SkippedCommands;
			}
		}
		return result;
	}

	DebugRenderer::DebugRenderer(ConstructionKey /*key*/)
		: m_State(CreateScope<State>())
	{
	}

	DebugRenderer::~DebugRenderer() = default;

	Result<Scope<DebugRenderer>> DebugRenderer::Create(GraphicsDevice& device, PipelineFactory& pipelines)
	{
		Scope<DebugRenderer> renderer = CreateScope<DebugRenderer>(ConstructionKey());
		State& state = *renderer->m_State;
		state.Device = &device;
		std::vector<PipelineLayoutDescription> descriptions = Utils::MakeDebugLinesDescriptions();
		// Both pipelines share one binding layout, so one binding set per view serves both.
		ENGINE_TRY_ASSIGN(const std::vector<nvrhi::BindingLayoutHandle> sharedLayouts,
			WithContext(pipelines.CreateBindingLayouts(descriptions[Utils::DebugLinesTestedPipeline]), "while creating the debug-line pipelines"));
		for (size_t index = 0; index < descriptions.size(); ++index)
		{
			const bool depthTested = index == Utils::DebugLinesTestedPipeline;
			ENGINE_TRY_ASSIGN(GraphicsPipeline pipeline,
				WithContext(pipelines.CreateGraphicsPipeline(Utils::MakeDebugLinesSpecification(std::move(descriptions[index]), depthTested, sharedLayouts)),
					"while creating the debug-line pipelines"));
			state.Pipelines.push_back(std::move(pipeline));
		}
		return renderer;
	}

	std::vector<PipelineLayoutDescription> DebugRenderer::GetLayoutDescriptions()
	{
		return Utils::MakeDebugLinesDescriptions();
	}

	uint32_t DebugRenderer::GetPipelineCount() const
	{
		return static_cast<uint32_t>(m_State->Pipelines.size());
	}

	Result<uint32_t> DebugRenderer::Record(nvrhi::ICommandList& commandList, PassBindingCache& bindings, const DebugRenderInputs& inputs)
	{
		ENGINE_CORE_ASSERT(inputs.DebugDraw != nullptr && inputs.Framebuffer != nullptr && inputs.ViewConstants != nullptr,
			"DebugRenderer::Record needs a debug draw list, a framebuffer and ViewConstants");
		State& state = *m_State;

		const DebugLineVertices vertices = BuildDebugLineVertices(*inputs.DebugDraw);
		if (vertices.BudgetSkippedCommands > 0 && !state.HasWarnedBudget)
		{
			state.HasWarnedBudget = true;
			ENGINE_CORE_WARN("The debug renderer left out {} debug draw command(s) whose lines exceed the budget of {} vertices per view; "
							 "further commands beyond the budget are left out without a warning",
				vertices.BudgetSkippedCommands, MaxDebugLineVertices);
		}
		const size_t testedCount = vertices.Tested.size();
		const size_t onTopCount = vertices.OnTop.size();
		const uint32_t total = static_cast<uint32_t>(testedCount + onTopCount);
		if (total == 0)
			return 0U;

		// Everything that can fail, before the first command.
		if (state.Vertices == nullptr || state.VertexCapacity < total)
		{
			const uint32_t capacity = Utils::GrowDebugLineCapacity(state.VertexCapacity, total);
			nvrhi::BufferDesc desc;
			desc.byteSize = static_cast<uint64_t>(capacity) * sizeof(DebugLineVertex);
			desc.isVertexBuffer = true;
			desc.initialState = nvrhi::ResourceStates::VertexBuffer;
			desc.keepInitialState = true;
			desc.debugName = "DebugRenderer.Vertices";
			ENGINE_TRY_ASSIGN(nvrhi::BufferHandle buffer, WithContext(state.Device->CreateBuffer(desc), "while recording the debug lines"));
			state.Vertices = std::move(buffer);
			state.VertexCapacity = capacity;
		}
		nvrhi::BindingSetDesc setDesc;
		setDesc.bindings = { nvrhi::BindingSetItem::ConstantBuffer(0, inputs.ViewConstants) };
		ENGINE_TRY_ASSIGN(nvrhi::IBindingSet * set,
			WithContext(bindings.GetOrCreate(*state.Device, setDesc, *state.Pipelines[Utils::DebugLinesTestedPipeline].BindingLayouts[0]),
				"while recording the debug lines"));

		commandList.beginMarker("DebugLines");
		if (testedCount > 0)
			commandList.writeBuffer(state.Vertices, vertices.Tested.data(), testedCount * sizeof(DebugLineVertex), 0);
		if (onTopCount > 0)
		{
			commandList.writeBuffer(state.Vertices, vertices.OnTop.data(), onTopCount * sizeof(DebugLineVertex),
				static_cast<uint64_t>(testedCount) * sizeof(DebugLineVertex));
		}

		const nvrhi::FramebufferInfoEx& target = inputs.Framebuffer->getFramebufferInfo();
		nvrhi::GraphicsState graphics;
		graphics.framebuffer = inputs.Framebuffer;
		graphics.viewport.addViewportAndScissorRect(nvrhi::Viewport(static_cast<float>(target.width), static_cast<float>(target.height)));
		graphics.bindings = { set };
		graphics.addVertexBuffer(nvrhi::VertexBufferBinding().setBuffer(state.Vertices).setSlot(0).setOffset(0));
		const std::array<std::pair<size_t, size_t>, 2> draws = { {
			{ Utils::DebugLinesTestedPipeline, testedCount },
			{ Utils::DebugLinesOnTopPipeline, onTopCount },
		} };
		uint32_t first = 0;
		for (const auto& [pipeline, count] : draws)
		{
			if (count == 0)
				continue;
			graphics.pipeline = state.Pipelines[pipeline].Pipeline;
			commandList.setGraphicsState(graphics);
			nvrhi::DrawArguments arguments;
			arguments.vertexCount = static_cast<uint32_t>(count);
			arguments.startVertexLocation = first;
			commandList.draw(arguments);
			first += static_cast<uint32_t>(count);
		}
		commandList.endMarker();
		return total;
	}

}
