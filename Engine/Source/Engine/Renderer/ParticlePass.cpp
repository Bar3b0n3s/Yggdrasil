#include "EnginePCH.h"
#include "Engine/Renderer/ParticlePass.h"

#include "Engine/Asset/BuiltinAssets.h"
#include "Engine/Core/Assert.h"
#include "Engine/Graphics/CommandListRecording.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Renderer/GpuResourceCache.h"
#include "Engine/Renderer/RenderStats.h"
#include "Engine/Renderer/SceneTargetFormats.h"
#include "Shared/ParticleVertex.h"
#include "Shared/ViewConstants.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// PlanParticlePass builds every emitter's quads in the snapshot's order; an Alpha emitter's are sorted by a key of (view
// depth, particle index), a total order, so the plan never depends on the sort's stability. ParticlePass::Record resolves
// each run's texture once per call, lays the quads out into one vertex list (four ParticleVertex per quad, in the corners'
// order) cut into batches wherever the texture or the blend changes and at every MaxQuadsPerBatch quads, then, chunk by
// chunk, writes the chunk's vertices at the start of the vertex buffer and records its batches' colour draws and, with a
// velocity framebuffer, their motion draws, before the next chunk's write (NVRHI's write tracks the buffer's state, so it
// waits for the draws before it). The index buffer holds MaxQuadsPerBatch quads' indices (0, 1, 2, 0, 2, 3 per quad),
// written once at creation.

namespace Engine {

	namespace Utils {

		constexpr std::string_view ParticlesProgram = "Particles";
		// Indices of the three pipelines (GetLayoutDescriptions' order).
		constexpr size_t ParticlesAlphaPipeline = 0;
		constexpr size_t ParticlesAdditivePipeline = 1;
		constexpr size_t ParticlesVelocityPipeline = 2;
		static_assert(ParticlesVelocityPipeline + 1 == ParticlePass::PipelineCount);
		constexpr uint32_t ParticleVerticesPerQuad = 4;
		constexpr uint32_t ParticleIndicesPerQuad = 6;
		constexpr std::array<uint32_t, ParticleIndicesPerQuad> ParticleCornerIndices = { 0, 1, 2, 0, 2, 3 };
		// Set 0's registers (Passes/Particles.slang).
		constexpr uint32_t ParticleViewConstantsSlot = 0;
		constexpr uint32_t ParticleTextureSlot = 0;
		constexpr uint32_t ParticleSamplerSlot = 0;
		constexpr uint32_t ParticleSceneDepthSlot = 1;
		// The slowest a Velocity particle moves across the view and still stretches, in metres per second.
		constexpr float MinParticleStretchSpeed = 1e-6f;

		// The clip volume's planes as (a, b, c, d) with a point (x, y, z, w) inside when ax + by + cz + dw >= 0: -w <= x <= w,
		// -w <= y <= w and 0 <= z <= w (reverse-Z, §8.3), as the Sprites stage culls.
		constexpr std::array<glm::vec4, 6> ParticleClipPlanes = { {
			{ 1.0f, 0.0f, 0.0f, 1.0f },
			{ -1.0f, 0.0f, 0.0f, 1.0f },
			{ 0.0f, 1.0f, 0.0f, 1.0f },
			{ 0.0f, -1.0f, 0.0f, 1.0f },
			{ 0.0f, 0.0f, 1.0f, 0.0f },
			{ 0.0f, 0.0f, -1.0f, 1.0f },
		} };

		// Whether the quad of clip-space corners `clip` lies wholly outside one plane of the clip volume.
		static bool IsParticleOutsideClipVolume(const std::array<glm::vec4, 4>& clip)
		{
			return std::ranges::any_of(ParticleClipPlanes, [&clip](const glm::vec4& plane)
			{
				return std::ranges::all_of(clip, [&plane](const glm::vec4& point)
				{
					return glm::dot(plane, point) < 0.0f;
				});
			});
		}

		static bool IsFiniteVector(const glm::vec3& value)
		{
			return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
		}

		static bool IsFiniteColor(const glm::vec4& value)
		{
			return std::isfinite(value.r) && std::isfinite(value.g) && std::isfinite(value.b) && std::isfinite(value.a);
		}

		// `axis` normalized; empty when it is not finite or has no length.
		static std::optional<glm::vec3> NormalizeAxis(const glm::vec3& axis)
		{
			if (!IsFiniteVector(axis))
				return std::nullopt;
			const float length = std::sqrt(glm::dot(axis, axis));
			if (!std::isfinite(length) || !(length > 0.0f))
				return std::nullopt;
			return axis / length;
		}

		// Row `row` of `view` (world to view) as a world-space direction normalized: row 0 is the camera's right axis and row 1
		// its up axis. Empty when the row is not finite or has no length.
		static std::optional<glm::vec3> GetViewAxis(const glm::mat4& view, glm::length_t row)
		{
			return NormalizeAxis(glm::vec3(view[0][row], view[1][row], view[2][row]));
		}

		// The axes `right` and `up` turned counter-clockwise (right toward up) by `angle` radians; unchanged at exactly 0, so
		// an unturned quad's corners are computed from the axes themselves.
		static std::pair<glm::vec3, glm::vec3> TurnAxes(const glm::vec3& right, const glm::vec3& up, float angle)
		{
			if (angle == 0.0f)
				return { right, up };
			const float cosine = std::cos(angle);
			const float sine = std::sin(angle);
			return { right * cosine + up * sine, up * cosine - right * sine };
		}

		// One kept particle of an emitter: its view depth and its index, the Alpha emitters' sort key.
		struct ParticleDepthKey
		{
			float Depth = 0.0f; // the view-space z of the particle's centre: more negative is farther
			uint32_t Index = 0; // the particle's index in the snapshot's Particles
		};

		// The texture rectangles of frame `phase` of `frames` (decision 5): frame floor(phase n) mod n and the next one, and,
		// with `blend`, the fraction toward the next while the next is still to play: the range plays `cycles` times over a
		// life, so the last frame of its last play is held rather than blended into the first.
		struct ParticleFrame
		{
			glm::vec4 TexCoords = glm::vec4(0.0f, 0.0f, 1.0f, 1.0f);
			glm::vec4 NextTexCoords = glm::vec4(0.0f, 0.0f, 1.0f, 1.0f);
			float Blend = 0.0f;
		};

		static ParticleFrame GetParticleFrame(std::span<const glm::vec4> frames, float phase, bool blend, float cycles)
		{
			const auto count = static_cast<uint64_t>(frames.size());
			// A negative or non-finite phase (no extraction makes one) shows the range's first frame; one past 2^52 frames (data
			// no extraction makes either) is held there, where a double still counts whole frames, so the cast below fits.
			constexpr double LargestFramePosition = 4503599627370496.0;
			const double position = std::isfinite(phase) && phase > 0.0f ? std::min(static_cast<double>(phase) * static_cast<double>(count), LargestFramePosition)
																		 : 0.0;
			const double whole = std::floor(position);
			const uint64_t frame = static_cast<uint64_t>(whole) % count;
			const bool nextPlays = std::isfinite(cycles) && whole + 1.0 < static_cast<double>(cycles) * static_cast<double>(count);
			return ParticleFrame{ .TexCoords = frames[frame],
				.NextTexCoords = frames[(frame + 1) % count],
				.Blend = blend && nextPlays ? static_cast<float>(position - whole) : 0.0f };
		}

		// How an emitter lays its particles' quads (the file comment): its alignment, the view's right and up axes, the Flat
		// plane's (when both are usable) and a Velocity quad's stretch.
		struct ParticleLayout
		{
			RenderParticleAlignment Alignment = RenderParticleAlignment::Camera;
			glm::vec3 Right = glm::vec3(1.0f, 0.0f, 0.0f);
			glm::vec3 Up = glm::vec3(0.0f, 1.0f, 0.0f);
			std::optional<std::pair<glm::vec3, glm::vec3>> Plane{};
			float Stretch = 0.0f;
		};

		// The corners of a quad drawn from `draw` (top-left, top-right, bottom-right, bottom-left), laid by `layout`.
		static std::array<glm::vec3, 4> LayParticleQuad(const ParticleLayout& layout, const ParticleDrawState& draw)
		{
			const float half = 0.5f * draw.Size;
			glm::vec3 across{ 0.0f };
			glm::vec3 above{ 0.0f };
			bool laid = false;
			if (layout.Alignment == RenderParticleAlignment::Velocity && IsFiniteVector(draw.Velocity))
			{
				const float alongRight = glm::dot(draw.Velocity, layout.Right);
				const float alongUp = glm::dot(draw.Velocity, layout.Up);
				const float speed = std::sqrt(alongRight * alongRight + alongUp * alongUp);
				if (std::isfinite(speed) && speed > MinParticleStretchSpeed)
				{
					const glm::vec3 along = (layout.Right * alongRight + layout.Up * alongUp) / speed;
					const glm::vec3 side = (layout.Right * alongUp - layout.Up * alongRight) / speed;
					across = side * half;
					above = along * (0.5f * (draw.Size + speed * layout.Stretch));
					laid = true;
				}
			}
			if (!laid)
			{
				// A Velocity particle's angle is unused, its fallback square included.
				const float angle = layout.Alignment == RenderParticleAlignment::Velocity ? 0.0f : draw.Rotation;
				const auto [right, up] = layout.Plane.has_value() ? TurnAxes(layout.Plane->first, layout.Plane->second, angle) : TurnAxes(layout.Right, layout.Up, angle);
				across = right * half;
				above = up * half;
			}
			return { draw.Position - across + above, draw.Position + across + above, draw.Position + across - above, draw.Position - across - above };
		}

		// Whether the pipeline blends additively.
		static bool IsAdditivePipeline(size_t pipeline)
		{
			return pipeline == ParticlesAdditivePipeline;
		}

		static PipelineLayoutDescription MakeParticleDescription(std::string name, std::string fragmentEntry)
		{
			nvrhi::BindingLayoutDesc layout;
			layout.visibility = nvrhi::ShaderType::All;
			layout.registerSpace = 0;
			layout.registerSpaceIsDescriptorSet = true;
			layout.bindings = {
				nvrhi::BindingLayoutItem::ConstantBuffer(ParticleViewConstantsSlot),
				nvrhi::BindingLayoutItem::Texture_SRV(ParticleTextureSlot),
				nvrhi::BindingLayoutItem::Texture_SRV(ParticleSceneDepthSlot),
				nvrhi::BindingLayoutItem::Sampler(ParticleSamplerSlot),
			};
			return {
				.Name = std::move(name),
				.Program = std::string(ParticlesProgram),
				.Entries = { "VSMain", std::move(fragmentEntry) },
				.Permutation = {},
				.BindingLayouts = { layout },
				.StorageImages = {},
				.ConstantBuffers = { { .Set = 0, .Register = ParticleViewConstantsSlot, .ByteSize = sizeof(ViewConstants) } },
			};
		}

		static std::vector<PipelineLayoutDescription> MakeParticleDescriptions()
		{
			return { MakeParticleDescription("ParticlesAlpha", "PSColor"), MakeParticleDescription("ParticlesAdditive", "PSColor"),
				MakeParticleDescription("ParticlesVelocity", "PSVelocity") };
		}

		static std::vector<nvrhi::VertexAttributeDesc> MakeParticleVertexAttributes()
		{
			constexpr uint32_t Stride = sizeof(ParticleVertex);
			const auto attribute = [](const char* name, nvrhi::Format format, size_t offset)
			{
				return nvrhi::VertexAttributeDesc().setName(name).setFormat(format).setOffset(static_cast<uint32_t>(offset)).setElementStride(Stride);
			};
			return {
				attribute("POSITION", nvrhi::Format::RGB32_FLOAT, offsetof(ParticleVertex, Position)),
				attribute("PREVIOUSPOSITION", nvrhi::Format::RGB32_FLOAT, offsetof(ParticleVertex, PreviousPosition)),
				attribute("COLOR", nvrhi::Format::RGBA32_FLOAT, offsetof(ParticleVertex, Color)),
				attribute("TEXCOORDS", nvrhi::Format::RGBA32_FLOAT, offsetof(ParticleVertex, TexCoords)),
				attribute("PARAMS", nvrhi::Format::RG32_FLOAT, offsetof(ParticleVertex, Params)),
			};
		}

		// A particle pipeline: double-sided quads without a depth attachment (the fragments test SceneDepth themselves). The
		// colour pipelines blend as the QuadBatcher's Hdr (straight alpha: colour SrcAlpha / InvSrcAlpha, alpha One /
		// InvSrcAlpha) and HdrAdditive (colour SrcAlpha / One, the target's alpha kept: Zero / One) did; the velocity one
		// replaces the motion where it covers.
		static GraphicsPipelineSpecification MakeParticleSpecification(PipelineLayoutDescription layout, size_t pipeline,
			const std::vector<nvrhi::BindingLayoutHandle>& sharedLayouts)
		{
			GraphicsPipelineSpecification specification;
			specification.Layout = std::move(layout);
			specification.SharedBindingLayouts = sharedLayouts;
			specification.VertexAttributes = MakeParticleVertexAttributes();
			specification.Primitive = nvrhi::PrimitiveType::TriangleList;
			nvrhi::BlendState::RenderTarget& blend = specification.RenderState.blendState.targets[0];
			if (IsAdditivePipeline(pipeline))
			{
				blend.enableBlend()
					.setSrcBlend(nvrhi::BlendFactor::SrcAlpha)
					.setDestBlend(nvrhi::BlendFactor::One)
					.setBlendOp(nvrhi::BlendOp::Add)
					.setSrcBlendAlpha(nvrhi::BlendFactor::Zero)
					.setDestBlendAlpha(nvrhi::BlendFactor::One)
					.setBlendOpAlpha(nvrhi::BlendOp::Add);
			}
			else if (pipeline == ParticlesAlphaPipeline)
			{
				blend.enableBlend()
					.setSrcBlend(nvrhi::BlendFactor::SrcAlpha)
					.setDestBlend(nvrhi::BlendFactor::InvSrcAlpha)
					.setBlendOp(nvrhi::BlendOp::Add)
					.setSrcBlendAlpha(nvrhi::BlendFactor::One)
					.setDestBlendAlpha(nvrhi::BlendFactor::InvSrcAlpha)
					.setBlendOpAlpha(nvrhi::BlendOp::Add);
			}
			specification.RenderState.rasterState.setCullNone().enableDepthClip();
			specification.RenderState.depthStencilState.setDepthTestEnable(false).setDepthWriteEnable(false).setStencilEnable(false);
			specification.Framebuffer = pipeline == ParticlesVelocityPipeline ? ParticlePass::GetVelocityFramebufferInfo() : ParticlePass::GetColorFramebufferInfo();
			return specification;
		}

	}

	namespace {

		// A run of consecutive quads drawn with one texture and one blend, all in one chunk of MaxQuadsPerBatch quads.
		struct ParticleBatch
		{
			nvrhi::ITexture* Texture = nullptr;
			RenderParticleBlend Blend = RenderParticleBlend::Alpha;
			uint32_t FirstQuad = 0;
			uint32_t QuadCount = 0;
		};

	}

	Status PlanParticlePass(std::span<const ParticleEmitterItem> emitters, std::span<const ParticleItem> particles,
		std::span<const ParticleFlipbook> flipbooks, const glm::mat4& view, const glm::mat4& viewProjection, ParticlePassPlan& plan)
	{
		plan.Quads.clear();
		plan.Runs.clear();
		plan.Culled = 0;
		plan.Skipped = 0;
		const std::optional<glm::vec3> rightAxis = Utils::GetViewAxis(view, 0);
		const std::optional<glm::vec3> upAxis = Utils::GetViewAxis(view, 1);
		if (!rightAxis.has_value() || !upAxis.has_value())
			return {};

		Status firstError{};
		std::vector<Utils::ParticleDepthKey> keys;
		for (size_t emitterIndex = 0; emitterIndex < emitters.size(); ++emitterIndex)
		{
			const ParticleEmitterItem& emitter = emitters[emitterIndex];
			if (emitter.First > particles.size() || emitter.Count > particles.size() - emitter.First)
			{
				if (firstError)
				{
					firstError = MakeError(ErrorCode::InvalidArgument,
						"particle emitter entity {} in the render snapshot names particles [{}, +{}) of {}; its particles are skipped", emitter.Entity,
						emitter.First, emitter.Count, particles.size());
				}
				continue;
			}
			// A flipbook that did not resolve draws nothing (decision 5).
			const ParticleFlipbook* flipbook = nullptr;
			if (emitter.Sheet.IsValid())
			{
				if (emitterIndex >= flipbooks.size() || flipbooks[emitterIndex].Frames.empty())
					continue;
				flipbook = &flipbooks[emitterIndex];
			}
			// The emitter's layout: the plane of a Flat emitter, or nothing (its particles then face the camera).
			Utils::ParticleLayout layout{ .Alignment = emitter.Alignment,
				.Right = *rightAxis,
				.Up = *upAxis,
				.Stretch = std::isfinite(emitter.VelocityStretch) ? std::max(emitter.VelocityStretch, 0.0f) : 0.0f };
			const std::optional<glm::vec3> planeRight = Utils::NormalizeAxis(emitter.PlaneRight);
			const std::optional<glm::vec3> planeUp = Utils::NormalizeAxis(emitter.PlaneUp);
			if (emitter.Alignment == RenderParticleAlignment::Flat && planeRight.has_value() && planeUp.has_value())
				layout.Plane = std::pair(*planeRight, *planeUp);
			const float inverseSoftDistance = std::isfinite(emitter.SoftDistance) && emitter.SoftDistance > 0.0f ? 1.0f / emitter.SoftDistance : 0.0f;

			keys.clear();
			const size_t end = static_cast<size_t>(emitter.First) + emitter.Count;
			for (size_t index = emitter.First; index < end; ++index)
			{
				const ParticleItem& particle = particles[index];
				if (!Utils::IsFiniteVector(particle.Position) || !Utils::IsFiniteColor(particle.Color) || !std::isfinite(particle.Size)
					|| !(particle.Size > 0.0f) || !std::isfinite(particle.Rotation))
				{
					++plan.Skipped;
					continue;
				}
				const glm::vec4 viewPosition = view * glm::vec4(particle.Position, 1.0f);
				keys.push_back({ .Depth = viewPosition.z, .Index = static_cast<uint32_t>(index) });
			}
			if (emitter.Blend == RenderParticleBlend::Alpha)
			{
				// Back to front: the farthest (most negative view z) first, ties in particle order.
				std::ranges::sort(keys, [](const Utils::ParticleDepthKey& left, const Utils::ParticleDepthKey& other)
				{
					return left.Depth != other.Depth ? left.Depth < other.Depth : left.Index < other.Index;
				});
			}

			const auto first = static_cast<uint32_t>(plan.Quads.size());
			for (const Utils::ParticleDepthKey& key : keys)
			{
				const ParticleItem& particle = particles[key.Index];
				ParticlePassQuad quad{
					.Corners = Utils::LayParticleQuad(layout,
						ParticleDrawState{ .Position = particle.Position, .Size = particle.Size, .Rotation = particle.Rotation, .Velocity = particle.Velocity }),
					.Color = particle.Color,
					.InverseSoftDistance = inverseSoftDistance,
				};
				// A size or position near the float range overflows a corner.
				if (!std::ranges::all_of(quad.Corners, &Utils::IsFiniteVector))
				{
					++plan.Skipped;
					continue;
				}
				std::array<glm::vec4, 4> clip{};
				for (size_t corner = 0; corner < quad.Corners.size(); ++corner)
					clip[corner] = viewProjection * glm::vec4(quad.Corners[corner], 1.0f);
				if (Utils::IsParticleOutsideClipVolume(clip))
				{
					++plan.Culled;
					continue;
				}
				if (flipbook != nullptr)
				{
					const Utils::ParticleFrame frame = Utils::GetParticleFrame(flipbook->Frames, particle.FramePhase, emitter.BlendFrames, emitter.FrameCycles);
					quad.TexCoords = frame.TexCoords;
					quad.NextTexCoords = frame.NextTexCoords;
					quad.FrameBlend = frame.Blend;
				}
				// The previous corners, laid from how the view's previous render drew the particle (its position, size, angle and
				// stretch), so its spin, growth and stretch reach the motion vectors; without a previous draw, the corners stood
				// still.
				quad.PreviousCorners = quad.Corners;
				if (particle.Previous.has_value())
				{
					const std::array<glm::vec3, 4> previous = Utils::LayParticleQuad(layout, *particle.Previous);
					if (std::ranges::all_of(previous, &Utils::IsFiniteVector))
						quad.PreviousCorners = previous;
				}
				plan.Quads.push_back(quad);
			}
			const auto count = static_cast<uint32_t>(plan.Quads.size()) - first;
			if (count == 0)
				continue;
			const AssetHandle texture = flipbook != nullptr ? flipbook->Texture
															: (emitter.Texture.IsValid() ? emitter.Texture : BuiltinAssetHandles::ParticleTexture);
			plan.Runs.push_back(ParticlePassRun{ .Texture = texture, .Blend = emitter.Blend, .First = first, .Count = count, .Entity = emitter.Entity });
		}
		return firstError;
	}

	struct ParticlePass::State
	{
		GraphicsDevice* Device = nullptr; // documented back-reference
		std::array<GraphicsPipeline, PipelineCount> Pipelines{};
		std::array<nvrhi::SamplerHandle, 2> Samplers{}; // QuadSampler's values
		nvrhi::BufferHandle Vertices{};
		nvrhi::BufferHandle Indices{};
		uint32_t VertexCapacity = 0; // quads
		// Reused by every Record; released after one beyond MaxQuadsPerBatch quads (Record).
		std::vector<ParticleVertex> Staging{};
	};

	ParticlePass::ParticlePass(ConstructionKey /*key*/)
		: m_State(CreateScope<State>())
	{
	}

	ParticlePass::~ParticlePass() = default;

	Result<Scope<ParticlePass>> ParticlePass::Create(GraphicsDevice& device, PipelineFactory& pipelines)
	{
		Scope<ParticlePass> pass = CreateScope<ParticlePass>(ConstructionKey());
		State& state = *pass->m_State;
		state.Device = &device;
		std::vector<PipelineLayoutDescription> descriptions = Utils::MakeParticleDescriptions();
		// The three pipelines share one binding layout, so one binding set per texture serves every draw of a view.
		ENGINE_TRY_ASSIGN(const std::vector<nvrhi::BindingLayoutHandle> sharedLayouts,
			WithContext(pipelines.CreateBindingLayouts(descriptions[Utils::ParticlesAlphaPipeline]), "while creating the particle pipelines"));
		for (size_t index = 0; index < descriptions.size(); ++index)
		{
			ENGINE_TRY_ASSIGN(state.Pipelines[index],
				WithContext(pipelines.CreateGraphicsPipeline(Utils::MakeParticleSpecification(std::move(descriptions[index]), index, sharedLayouts)),
					"while creating the particle pipelines"));
		}
		ENGINE_TRY_ASSIGN(state.Samplers[static_cast<size_t>(QuadSampler::Point)],
			WithContext(device.CreateSampler(nvrhi::SamplerDesc().setAllFilters(false).setAllAddressModes(nvrhi::SamplerAddressMode::Clamp)),
				"while creating the particle pass"));
		ENGINE_TRY_ASSIGN(state.Samplers[static_cast<size_t>(QuadSampler::Linear)],
			WithContext(device.CreateSampler(nvrhi::SamplerDesc().setAllFilters(true).setMipFilter(false).setAllAddressModes(nvrhi::SamplerAddressMode::Clamp)),
				"while creating the particle pass"));

		nvrhi::BufferDesc indexDesc;
		indexDesc.byteSize = static_cast<uint64_t>(MaxQuadsPerBatch) * Utils::ParticleIndicesPerQuad * sizeof(uint32_t);
		indexDesc.isIndexBuffer = true;
		indexDesc.initialState = nvrhi::ResourceStates::IndexBuffer;
		indexDesc.keepInitialState = true;
		indexDesc.debugName = "ParticlePass.Indices";
		ENGINE_TRY_ASSIGN(state.Indices, WithContext(device.CreateBuffer(indexDesc), "while creating the particle pass"));
		std::vector<uint32_t> indices;
		indices.reserve(static_cast<size_t>(MaxQuadsPerBatch) * Utils::ParticleIndicesPerQuad);
		for (uint32_t quad = 0; quad < MaxQuadsPerBatch; ++quad)
		{
			for (const uint32_t corner : Utils::ParticleCornerIndices)
				indices.push_back(quad * Utils::ParticleVerticesPerQuad + corner);
		}
		// Not an immediate command list: a caller may have the frame's immediate list open (NVRHI's validation allows one).
		ENGINE_TRY_ASSIGN(const nvrhi::CommandListHandle commandList,
			WithContext(device.CreateCommandList(nvrhi::CommandListParameters().setEnableImmediateExecution(false)), "while creating the particle pass"));
		CommandListRecording recording(device, *commandList);
		commandList->writeBuffer(state.Indices, indices.data(), indices.size() * sizeof(uint32_t), 0);
		recording.Execute();
		return pass;
	}

	std::vector<PipelineLayoutDescription> ParticlePass::GetLayoutDescriptions()
	{
		return Utils::MakeParticleDescriptions();
	}

	nvrhi::FramebufferInfo ParticlePass::GetColorFramebufferInfo()
	{
		nvrhi::FramebufferInfo info;
		info.colorFormats = { SceneColorFormat };
		return info;
	}

	nvrhi::FramebufferInfo ParticlePass::GetVelocityFramebufferInfo()
	{
		nvrhi::FramebufferInfo info;
		info.colorFormats = { SceneVelocityFormat };
		return info;
	}

	uint32_t ParticlePass::GetPipelineCount() const
	{
		return static_cast<uint32_t>(std::ranges::count_if(m_State->Pipelines, [](const GraphicsPipeline& pipeline)
		{
			return pipeline.Pipeline != nullptr;
		}));
	}

	Result<uint32_t> ParticlePass::Record(nvrhi::ICommandList& commandList, PassBindingCache& bindings, const ParticlePassInputs& inputs,
		RenderPassCounters& counters)
	{
		ENGINE_CORE_ASSERT(inputs.Plan != nullptr && inputs.Resources != nullptr && inputs.ColorFramebuffer != nullptr && inputs.ViewConstants != nullptr
				&& inputs.SceneDepth != nullptr,
			"ParticlePass::Record needs a plan, the GpuResourceCache, a colour framebuffer, the view constants and SceneDepth");
		if (inputs.Plan == nullptr || inputs.Resources == nullptr || inputs.ColorFramebuffer == nullptr || inputs.ViewConstants == nullptr
			|| inputs.SceneDepth == nullptr)
		{
			return MakeError(ErrorCode::InvalidArgument,
				"the Particles pass needs a plan, the GpuResourceCache, a colour framebuffer, the view constants and SceneDepth");
		}
		State& state = *m_State;
		const ParticlePassPlan& plan = *inputs.Plan;

		// The textures of this record, each resolved once and held until it ends (a mirror reference is valid only until the
		// cache's next upload, the texture object as long as a handle holds it).
		std::vector<std::pair<AssetHandle, nvrhi::TextureHandle>> textures;
		const auto resolveTexture = [&textures, &inputs](AssetHandle handle) -> nvrhi::ITexture*
		{
			if (const auto found = std::ranges::find(textures, handle, &std::pair<AssetHandle, nvrhi::TextureHandle>::first); found != textures.end())
				return found->second.Get();
			return textures.emplace_back(handle, inputs.Resources->GetTexture(handle).Texture).second.Get();
		};

		// Lay out every run's quads, batched by texture and blend within chunks of MaxQuadsPerBatch quads.
		state.Staging.clear();
		std::vector<ParticleBatch> batches;
		for (const ParticlePassRun& run : plan.Runs)
		{
			const bool inside = static_cast<size_t>(run.First) + run.Count <= plan.Quads.size();
			ENGINE_CORE_ASSERT(inside, "a particle run's quads [{}, +{}) lie outside the plan's {} quads", run.First, run.Count, plan.Quads.size());
			if (!inside)
				continue;
			// Not even the placeholder's mirror could be created (reported by the cache): the run is skipped rather than drawn
			// as white squares.
			nvrhi::ITexture* texture = resolveTexture(run.Texture);
			if (texture == nullptr)
				continue;
			for (const ParticlePassQuad& quad : std::span<const ParticlePassQuad>(plan.Quads).subspan(run.First, run.Count))
			{
				const std::array<glm::vec2, 4> texCoords = {
					glm::vec2(quad.TexCoords.x, quad.TexCoords.y),
					glm::vec2(quad.TexCoords.z, quad.TexCoords.y),
					glm::vec2(quad.TexCoords.z, quad.TexCoords.w),
					glm::vec2(quad.TexCoords.x, quad.TexCoords.w),
				};
				const std::array<glm::vec2, 4> nextTexCoords = {
					glm::vec2(quad.NextTexCoords.x, quad.NextTexCoords.y),
					glm::vec2(quad.NextTexCoords.z, quad.NextTexCoords.y),
					glm::vec2(quad.NextTexCoords.z, quad.NextTexCoords.w),
					glm::vec2(quad.NextTexCoords.x, quad.NextTexCoords.w),
				};
				for (size_t corner = 0; corner < quad.Corners.size(); ++corner)
				{
					state.Staging.push_back(ParticleVertex{ .Position = quad.Corners[corner],
						.PreviousPosition = quad.PreviousCorners[corner],
						.Color = quad.Color,
						.TexCoords = glm::vec4(texCoords[corner], nextTexCoords[corner]),
						.Params = glm::vec2(quad.FrameBlend, quad.InverseSoftDistance) });
				}
				const auto index = static_cast<uint32_t>(state.Staging.size() / Utils::ParticleVerticesPerQuad) - 1;
				if (!batches.empty() && batches.back().Texture == texture && batches.back().Blend == run.Blend && index % MaxQuadsPerBatch != 0)
					++batches.back().QuadCount;
				else
					batches.push_back({ .Texture = texture, .Blend = run.Blend, .FirstQuad = index, .QuadCount = 1 });
			}
		}
		const size_t quadTotal = state.Staging.size() / Utils::ParticleVerticesPerQuad;
		Result<uint32_t> result = static_cast<uint32_t>(quadTotal);
		if (quadTotal > 0)
		{
			// Everything that can fail before the first command: the binding sets, then the vertex buffer.
			nvrhi::IBindingLayout& layout = *state.Pipelines[Utils::ParticlesAlphaPipeline].BindingLayouts[0];
			const auto samplerIndex = static_cast<size_t>(inputs.Sampler);
			ENGINE_CORE_ASSERT(samplerIndex < state.Samplers.size(), "unknown quad sampler {}", samplerIndex);
			nvrhi::ISampler* sampler = state.Samplers[std::min(samplerIndex, state.Samplers.size() - 1)].Get();
			std::vector<nvrhi::IBindingSet*> sets;
			sets.reserve(batches.size());
			Status prepared{};
			for (const ParticleBatch& batch : batches)
			{
				nvrhi::BindingSetDesc desc;
				desc.bindings = {
					nvrhi::BindingSetItem::ConstantBuffer(Utils::ParticleViewConstantsSlot, inputs.ViewConstants),
					nvrhi::BindingSetItem::Texture_SRV(Utils::ParticleTextureSlot, batch.Texture),
					nvrhi::BindingSetItem::Texture_SRV(Utils::ParticleSceneDepthSlot, inputs.SceneDepth),
					nvrhi::BindingSetItem::Sampler(Utils::ParticleSamplerSlot, sampler),
				};
				Result<nvrhi::IBindingSet*> set = WithContext(bindings.GetOrCreate(*state.Device, desc, layout), "while recording the particles");
				if (!set)
				{
					prepared = std::unexpected(set.error());
					break;
				}
				sets.push_back(*set);
			}
			const auto largestChunk = static_cast<uint32_t>(std::min(quadTotal, static_cast<size_t>(MaxQuadsPerBatch)));
			if (prepared && (state.Vertices == nullptr || state.VertexCapacity < largestChunk))
			{
				nvrhi::BufferDesc desc;
				desc.byteSize = static_cast<uint64_t>(largestChunk) * Utils::ParticleVerticesPerQuad * sizeof(ParticleVertex);
				desc.isVertexBuffer = true;
				desc.initialState = nvrhi::ResourceStates::VertexBuffer;
				desc.keepInitialState = true;
				desc.debugName = "ParticlePass.Vertices";
				Result<nvrhi::BufferHandle> buffer = WithContext(state.Device->CreateBuffer(desc), "while recording the particles");
				if (buffer)
				{
					state.Vertices = std::move(*buffer);
					state.VertexCapacity = largestChunk;
				}
				else
					prepared = std::unexpected(buffer.error());
			}
			if (!prepared)
				result = std::unexpected(prepared.error());
			else
			{
				const nvrhi::FramebufferInfoEx& target = inputs.ColorFramebuffer->getFramebufferInfo();
				const uint32_t width = std::max(target.width, 1U);
				const uint32_t height = std::max(target.height, 1U);
				commandList.beginMarker("Particles");
				nvrhi::GraphicsState graphics;
				graphics.viewport.addViewport(nvrhi::Viewport(static_cast<float>(width), static_cast<float>(height)));
				graphics.viewport.addScissorRect(nvrhi::Rect(0, static_cast<int>(width), 0, static_cast<int>(height)));
				graphics.bindings.push_back(nullptr);
				graphics.addVertexBuffer(nvrhi::VertexBufferBinding().setBuffer(state.Vertices).setSlot(0).setOffset(0));
				graphics.setIndexBuffer(nvrhi::IndexBufferBinding().setBuffer(state.Indices).setFormat(nvrhi::Format::R32_UINT).setOffset(0));
				// Draws batches [begin, end) of the current chunk (starting at quad `chunkStart`) with `pipelineOf` into
				// `framebuffer`, setting the graphics state at the first draw and wherever the pipeline or the set changes.
				const auto drawBatches = [&](size_t begin, size_t end, size_t chunkStart, nvrhi::IFramebuffer* framebuffer, auto pipelineOf)
				{
					bool stateSet = false;
					for (size_t index = begin; index < end; ++index)
					{
						const ParticleBatch& batch = batches[index];
						nvrhi::IGraphicsPipeline* pipeline = state.Pipelines[pipelineOf(batch)].Pipeline.Get();
						if (!stateSet || graphics.pipeline != pipeline || graphics.bindings[0] != sets[index])
						{
							stateSet = true;
							graphics.pipeline = pipeline;
							graphics.framebuffer = framebuffer;
							graphics.bindings[0] = sets[index];
							commandList.setGraphicsState(graphics);
						}
						const auto firstInChunk = static_cast<uint32_t>(batch.FirstQuad - chunkStart);
						commandList.drawIndexed(nvrhi::DrawArguments()
								.setVertexCount(batch.QuadCount * Utils::ParticleIndicesPerQuad)
								.setStartIndexLocation(firstInChunk * Utils::ParticleIndicesPerQuad));
						++counters.DrawCalls;
						counters.Triangles += batch.QuadCount * 2;
					}
				};
				size_t first = 0;
				for (size_t chunkStart = 0; chunkStart < quadTotal; chunkStart += MaxQuadsPerBatch)
				{
					const size_t chunkQuads = std::min(quadTotal - chunkStart, static_cast<size_t>(MaxQuadsPerBatch));
					commandList.writeBuffer(state.Vertices, state.Staging.data() + chunkStart * Utils::ParticleVerticesPerQuad,
						chunkQuads * Utils::ParticleVerticesPerQuad * sizeof(ParticleVertex), 0);
					size_t last = first;
					while (last < batches.size() && batches[last].FirstQuad < chunkStart + chunkQuads)
						++last;
					drawBatches(first, last, chunkStart, inputs.ColorFramebuffer, [](const ParticleBatch& batch)
					{
						return batch.Blend == RenderParticleBlend::Additive ? Utils::ParticlesAdditivePipeline : Utils::ParticlesAlphaPipeline;
					});
					if (inputs.VelocityFramebuffer != nullptr)
					{
						drawBatches(first, last, chunkStart, inputs.VelocityFramebuffer, [](const ParticleBatch& /*batch*/)
						{
							return Utils::ParticlesVelocityPipeline;
						});
					}
					first = last;
				}
				commandList.endMarker();
			}
		}
		// Between records the staging keeps room for at most one chunk, the vertex buffer's capacity (as the QuadBatcher's).
		if (state.Staging.capacity() > static_cast<size_t>(MaxQuadsPerBatch) * Utils::ParticleVerticesPerQuad)
			std::vector<ParticleVertex>().swap(state.Staging);
		return result;
	}

}
