#include "EnginePCH.h"
#include "Engine/Renderer/ParticlePass.h"

#include "Engine/Asset/BuiltinAssets.h"
#include "Engine/Core/Assert.h"
#include "Engine/Renderer/GpuResourceCache.h"
#include "Engine/Renderer/RenderStats.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

// PlanParticlePass builds every emitter's quads in the snapshot's order; an Alpha emitter's are sorted by a key of (view
// depth, particle index), a total order, so the plan never depends on the sort's stability. RecordParticlePass resolves each
// run's texture once per call and records maximal sequences of runs of one blend as one QuadBatcher record, which splits its
// draws where the texture changes.

namespace Engine {

	namespace Utils {

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

		// Row `row` of `view` (world to view) as a world-space direction normalized: row 0 is the camera's right axis and row 1
		// its up axis. Empty when the row is not finite or has no length.
		static std::optional<glm::vec3> GetViewAxis(const glm::mat4& view, glm::length_t row)
		{
			const glm::vec3 axis(view[0][row], view[1][row], view[2][row]);
			if (!IsFiniteVector(axis))
				return std::nullopt;
			const float length = std::sqrt(glm::dot(axis, axis));
			if (!std::isfinite(length) || !(length > 0.0f))
				return std::nullopt;
			return axis / length;
		}

		// One kept particle of an emitter: its view depth and its index, the Alpha emitters' sort key.
		struct ParticleDepthKey
		{
			float Depth = 0.0f; // the view-space z of the particle's centre: more negative is farther
			uint32_t Index = 0; // the particle's index in the snapshot's Particles
		};

	}

	Status PlanParticlePass(std::span<const ParticleEmitterItem> emitters, std::span<const ParticleItem> particles, const glm::mat4& view,
		const glm::mat4& viewProjection, ParticlePassPlan& plan)
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
		for (const ParticleEmitterItem& emitter : emitters)
		{
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
			keys.clear();
			const size_t end = static_cast<size_t>(emitter.First) + emitter.Count;
			for (size_t index = emitter.First; index < end; ++index)
			{
				const ParticleItem& particle = particles[index];
				if (!Utils::IsFiniteVector(particle.Position) || !Utils::IsFiniteColor(particle.Color) || !std::isfinite(particle.Size)
					|| !(particle.Size > 0.0f))
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
				const float half = 0.5f * particle.Size;
				const glm::vec3 across = *rightAxis * half;
				const glm::vec3 above = *upAxis * half;
				const ParticlePassQuad quad{
					.Corners = { particle.Position - across + above, particle.Position + across + above, particle.Position + across - above,
						particle.Position - across - above },
					.Color = particle.Color,
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
				plan.Quads.push_back(quad);
			}
			const auto count = static_cast<uint32_t>(plan.Quads.size()) - first;
			if (count == 0)
				continue;
			plan.Runs.push_back(ParticlePassRun{ .Texture = emitter.Texture.IsValid() ? emitter.Texture : BuiltinAssetHandles::ParticleTexture,
				.Blend = emitter.Blend,
				.First = first,
				.Count = count,
				.Entity = emitter.Entity });
		}
		return firstError;
	}

	Result<uint32_t> RecordParticlePass(nvrhi::ICommandList& commandList, PassBindingCache& bindings, const ParticlePassInputs& inputs,
		std::vector<QuadDraw>& quads, RenderPassCounters& counters)
	{
		ENGINE_CORE_ASSERT(inputs.Plan != nullptr && inputs.Quads != nullptr && inputs.Resources != nullptr && inputs.Framebuffer != nullptr,
			"RecordParticlePass needs a plan, the QuadBatcher, the GpuResourceCache and a framebuffer");
		if (inputs.Plan == nullptr || inputs.Quads == nullptr || inputs.Resources == nullptr || inputs.Framebuffer == nullptr)
			return MakeError(ErrorCode::InvalidArgument, "the Particles pass needs a plan, the QuadBatcher, the GpuResourceCache and a framebuffer");
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

		Status firstError{};
		uint32_t drawn = 0;
		size_t position = 0;
		while (position < plan.Runs.size())
		{
			// The runs of one blend from `position` on, one record.
			const RenderParticleBlend blend = plan.Runs[position].Blend;
			quads.clear();
			for (; position < plan.Runs.size() && plan.Runs[position].Blend == blend; ++position)
			{
				const ParticlePassRun& run = plan.Runs[position];
				const bool inside = static_cast<size_t>(run.First) + run.Count <= plan.Quads.size();
				ENGINE_CORE_ASSERT(inside, "a particle run's quads [{}, +{}) lie outside the plan's {} quads", run.First, run.Count, plan.Quads.size());
				if (!inside)
					continue;
				// Not even the placeholder's mirror could be created (reported by the cache): the run is skipped rather than
				// drawn as white squares.
				nvrhi::ITexture* texture = resolveTexture(run.Texture);
				if (texture == nullptr)
					continue;
				for (const ParticlePassQuad& quad : std::span<const ParticlePassQuad>(plan.Quads).subspan(run.First, run.Count))
				{
					quads.push_back(QuadDraw{ .Corners = quad.Corners,
						.TexCoordMin = glm::vec2(0.0f),
						.TexCoordMax = glm::vec2(1.0f),
						.Color = quad.Color,
						.Texture = texture,
						.Sampler = inputs.Sampler });
				}
			}
			if (quads.empty())
				continue;
			const QuadBatchInputs batch{ .Quads = quads,
				.Target = blend == RenderParticleBlend::Additive ? QuadBatchTarget::HdrAdditive : QuadBatchTarget::Hdr,
				.Framebuffer = inputs.Framebuffer,
				.Transform = inputs.ViewProjection };
			const Result<uint32_t> recorded = inputs.Quads->Record(commandList, bindings, batch, counters);
			if (recorded)
				drawn += *recorded;
			else if (firstError)
				firstError = std::unexpected(recorded.error());
		}
		// The borrowed texture pointers are valid for this call only.
		quads.clear();
		ENGINE_TRY(firstError);
		return drawn;
	}

}
