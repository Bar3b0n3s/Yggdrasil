#include "EnginePCH.h"
#include "Engine/Renderer/TemporalHistory.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numeric>

// The view history of the temporal-state section (TemporalHistory.h; M26, C26.1, R-TAA-VEL, R-TAA; Docs/Decisions/
// 0037-m26-contract.md decisions 10 and 11; stream X-C). The jitter and the warm-up rule are pure; Apply reads only the
// snapshot and the history's own state.

namespace Engine {

	namespace Utils {

		// The Halton radical inverse of `index` in `base` as a numerator and a denominator, both exact in uint32_t for the
		// sequence's small indices: the digits of `index` mirrored about the radix point.
		static float HaltonRadicalInverse(uint32_t index, uint32_t base)
		{
			uint32_t numerator = 0;
			uint32_t denominator = 1;
			for (uint32_t rest = index; rest > 0; rest /= base)
			{
				numerator = numerator * base + rest % base;
				denominator *= base;
			}
			return static_cast<float>(numerator) / static_cast<float>(denominator);
		}

		static bool IsFinite(const glm::mat4& matrix)
		{
			for (int column = 0; column < 4; ++column)
			{
				for (int row = 0; row < 4; ++row)
				{
					if (!std::isfinite(matrix[column][row]))
						return false;
				}
			}
			return true;
		}

		// Whether every value of a particle's recorded draw is finite.
		static bool IsFiniteDraw(const ParticleDrawState& draw)
		{
			return std::isfinite(draw.Position.x) && std::isfinite(draw.Position.y) && std::isfinite(draw.Position.z) && std::isfinite(draw.Size)
				&& std::isfinite(draw.Rotation) && std::isfinite(draw.Velocity.x) && std::isfinite(draw.Velocity.y) && std::isfinite(draw.Velocity.z);
		}

		// Whether `camera` is another camera than `previous` for the history: another entity, projection kind or viewport.
		static bool IsCut(const CameraData& camera, const CameraData& previous)
		{
			return camera.Entity != previous.Entity || camera.ProjectionKind != previous.ProjectionKind || camera.ViewportWidth != previous.ViewportWidth
				|| camera.ViewportHeight != previous.ViewportHeight;
		}

	}

	glm::vec2 GetTaaJitter(uint32_t frame)
	{
		const uint32_t index = frame % TaaJitterSequenceLength + 1;
		return glm::vec2(Utils::HaltonRadicalInverse(index, 2) - 0.5f, Utils::HaltonRadicalInverse(index, 3) - 0.5f);
	}

	bool NeedsTemporalWarmup(const RenderSnapshot& snapshot)
	{
		// The renderer's rule for its temporal passes (SceneRenderer.cpp's Taa and Exposure): a Lit view with a camera, outside
		// the wireframe and pixel-art mode.
		if (!snapshot.HasCamera || snapshot.DebugView != RenderDebugView::Lit || HasFlag(snapshot.Flags, RenderViewFlags::Wireframe) || snapshot.Output.PixelArt)
			return false;
		return snapshot.Post.AntiAliasing == RenderAntiAliasing::Taa || snapshot.Post.ExposureMode == RenderExposureMode::Auto;
	}

	void RenderViewHistory::Apply(RenderSnapshot& snapshot, float deltaTime)
	{
		CameraData& camera = snapshot.Camera;
		const bool reset = m_ResetPending || !snapshot.HasCamera || !m_Camera.has_value() || Utils::IsCut(camera, *m_Camera);
		if (reset)
		{
			m_Frame = 0;
			m_Worlds.clear();
			m_Particles.clear();
			camera.PreviousViewProjection.reset();
			camera.DeltaTime = 0.0f;
		}
		else
		{
			++m_Frame;
			camera.PreviousViewProjection = m_Camera->Projection * m_Camera->View;
			camera.DeltaTime = std::isfinite(deltaTime) && deltaTime > 0.0f ? deltaTime : 0.0f;
		}
		camera.Jitter = snapshot.Post.AntiAliasing == RenderAntiAliasing::Taa && !snapshot.Output.PixelArt ? GetTaaJitter(m_Frame) : glm::vec2(0.0f);

		// The items' previous worlds by Entity: the items of each entity, in snapshot order, take the previous frame's worlds of
		// that entity in its order (both lists stably sorted by Entity, then merged); a previous world that is not finite, or
		// one the previous frame did not have, leaves the item without one (no motion).
		auto& items = snapshot.Meshes;
		std::vector<size_t> order(items.size());
		std::iota(order.begin(), order.end(), size_t{ 0 });
		std::ranges::stable_sort(order, [&items](size_t left, size_t right)
		{
			return items[left].Entity < items[right].Entity;
		});
		size_t previous = 0;
		for (size_t first = 0; first < order.size();)
		{
			const UUID entity = items[order[first]].Entity;
			size_t last = first;
			while (last < order.size() && items[order[last]].Entity == entity)
				++last;
			while (previous < m_Worlds.size() && m_Worlds[previous].first < entity)
				++previous;
			for (size_t position = first; position < last; ++position)
			{
				MeshDrawItem& item = items[order[position]];
				item.PreviousWorld.reset();
				if (previous < m_Worlds.size() && m_Worlds[previous].first == entity)
				{
					if (Utils::IsFinite(m_Worlds[previous].second))
						item.PreviousWorld = m_Worlds[previous].second;
					++previous;
				}
			}
			first = last;
		}

		// M33 (decision 6): each particle's previous draw, by its emitter's entity and incarnation and its serial. The previous
		// particles are sorted by that key, and an emitter's particles are in emission order, so their serials ascend: one
		// lookup of the emitter's block, then a merge.
		const auto byKey = [](const ParticleRecord& entry, const ParticleRecord& key)
		{
			if (entry.Emitter != key.Emitter)
				return entry.Emitter < key.Emitter;
			if (entry.Incarnation != key.Incarnation)
				return entry.Incarnation < key.Incarnation;
			return entry.Serial < key.Serial;
		};
		const auto sameState = [](const ParticleRecord& entry, const ParticleEmitterItem& emitter)
		{
			return entry.Emitter == emitter.Entity && entry.Incarnation == emitter.Incarnation;
		};
		for (const ParticleEmitterItem& emitter : snapshot.ParticleEmitters)
		{
			if (emitter.First > snapshot.Particles.size() || emitter.Count > snapshot.Particles.size() - emitter.First)
				continue;
			auto match = std::lower_bound(m_Particles.begin(), m_Particles.end(), ParticleRecord{ .Emitter = emitter.Entity, .Incarnation = emitter.Incarnation }, byKey);
			for (uint32_t index = emitter.First; index < emitter.First + emitter.Count; ++index)
			{
				ParticleItem& particle = snapshot.Particles[index];
				particle.Previous.reset();
				while (match != m_Particles.end() && sameState(*match, emitter) && match->Serial < particle.Serial)
					++match;
				if (match != m_Particles.end() && sameState(*match, emitter) && match->Serial == particle.Serial && Utils::IsFiniteDraw(match->Draw))
					particle.Previous = match->Draw;
			}
		}

		// This frame becomes the next Apply's previous one.
		if (snapshot.HasCamera)
			m_Camera = camera;
		else
			m_Camera.reset();
		m_Worlds.clear();
		m_Worlds.reserve(order.size());
		for (const size_t index : order)
			m_Worlds.emplace_back(items[index].Entity, items[index].World);
		// The emitters' blocks in key order (each block already ascends by serial, so only the blocks are sorted); an outsized
		// frame's capacity is released afterwards.
		std::vector<size_t> blocks;
		blocks.reserve(snapshot.ParticleEmitters.size());
		for (size_t index = 0; index < snapshot.ParticleEmitters.size(); ++index)
		{
			const ParticleEmitterItem& emitter = snapshot.ParticleEmitters[index];
			if (emitter.First <= snapshot.Particles.size() && emitter.Count <= snapshot.Particles.size() - emitter.First)
				blocks.push_back(index);
		}
		std::ranges::sort(blocks, [&snapshot](size_t left, size_t right)
		{
			const ParticleEmitterItem& first = snapshot.ParticleEmitters[left];
			const ParticleEmitterItem& second = snapshot.ParticleEmitters[right];
			return first.Entity != second.Entity ? first.Entity < second.Entity : first.Incarnation < second.Incarnation;
		});
		m_Particles.clear();
		for (const size_t block : blocks)
		{
			const ParticleEmitterItem& emitter = snapshot.ParticleEmitters[block];
			const size_t begin = m_Particles.size();
			for (uint32_t index = emitter.First; index < emitter.First + emitter.Count; ++index)
			{
				const ParticleItem& particle = snapshot.Particles[index];
				m_Particles.push_back({ .Emitter = emitter.Entity,
					.Incarnation = emitter.Incarnation,
					.Serial = particle.Serial,
					.Draw = { .Position = particle.Position, .Size = particle.Size, .Rotation = particle.Rotation, .Velocity = particle.Velocity } });
			}
			// Snapshot data no extraction makes (serials out of order) is sorted, so the lookups above stay valid.
			const auto first = m_Particles.begin() + static_cast<std::ptrdiff_t>(begin);
			if (!std::is_sorted(first, m_Particles.end(), byKey))
				std::sort(first, m_Particles.end(), byKey);
		}
		if (m_Particles.capacity() > std::max<size_t>(4 * m_Particles.size(), MaxRetainedParticleRecords))
			std::vector<ParticleRecord>(m_Particles.begin(), m_Particles.end()).swap(m_Particles);
		m_ResetPending = false;
	}

	void RenderViewHistory::Reset()
	{
		m_ResetPending = true;
	}

}
