#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/UUID.h"
#include "Engine/Renderer/RenderSnapshot.h"

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

// The temporal-state section's view history (M26, C26.1, R-TAA-VEL, R-TAA; Docs/Decisions/0037-m26-contract.md decisions 10
// and 11): what one view remembers between its renders, so the snapshot carries the previous-frame data that motion vectors,
// TAA and auto-exposure need (ADR 0028's phase-3 ledger: "the previous view is snapshot data"), while the renderer keeps
// only its per-view GPU history (TAA's history image, the adapted exposure), which it starts afresh whenever the snapshot
// says the history was reset.
//
// One RenderViewHistory per view that renders over time: each editor view (scene and game), the Runtime's window, the
// FeatureTest runner's view, and every capture (ViewportCapture, which resets it and renders the warm-up). Apply(snapshot,
// deltaTime) is called on each extracted snapshot right before its Render:
//   - when the history is empty or Reset was called since its last Apply, the snapshot has no camera, or the camera changed
//     its projection kind, its viewport size or its entity (a cut), the snapshot's history is reset:
//     CameraData::PreviousViewProjection, every MeshDrawItem::PreviousWorld and every ParticleItem::Previous are emptied and
//     DeltaTime is 0;
//   - otherwise PreviousViewProjection is the previous Apply's unjittered Projection * View, each mesh item's PreviousWorld
//     is the previous Apply's World of the item with the same Entity (empty for an item it did not see, or whose previous
//     World is not finite; items of one entity matched in order), each particle's Previous is how the previous Apply drew the
//     particle with the same emitter entity and incarnation and the same Serial: its position, size, angle and velocity (M33,
//     Docs/Decisions/0048-m33-particle-look.md decision 6: empty for a particle it did not see, or whose recorded values are
//     not all finite), and DeltaTime is `deltaTime` (0 when it is negative or not finite);
//   - Jitter is GetTaaJitter(frame) when the snapshot's Post.AntiAliasing is Taa and its Output is not PixelArt, else (0, 0),
//     where frame counts the Applies since the last reset (0 at the reset);
//   - then it records the snapshot's camera, items and particles as the next Apply's previous frame.
// Deterministic: it reads only the snapshot and its own state, never the wall clock; its matching is by Entity through a
// sorted vector (never an unordered container's iteration).
//
// Captures (decision 10: "captures reset history and render a fixed warm-up"): a capture of a snapshot whose post-processing
// is temporal (NeedsTemporalWarmup) resets its history and renders the same snapshot CaptureWarmupFrames times with
// deltaTime 0, keeping only the last image, so TAA has resolved its jitter sequence and auto-exposure converged (at once,
// after the reset), and two captures of one scene are byte-identical. A snapshot without temporal post-processing renders
// once, as before M26.
//
// Frozen by the M26 contract C26.1; stream X-C implements it (and the hosts' calls: ViewportCapture's warm-up, the Runtime's
// game view, the editor's scene and game views, EditorHostViewports).

namespace Engine {

	// TAA's jitter sequence: Halton (2, 3), TaaJitterSequenceLength samples, repeated.
	inline constexpr uint32_t TaaJitterSequenceLength = 8;
	// M33 (Docs/Decisions/0048-m33-particle-look.md decision 6): the particle records a history keeps room for after any frame;
	// beyond it, and beyond four times the frame's own count, an outsized frame's capacity is released.
	inline constexpr size_t MaxRetainedParticleRecords = 4096;

	// The renders of a capture with temporal post-processing: the whole jitter sequence once.
	inline constexpr uint32_t CaptureWarmupFrames = TaaJitterSequenceLength;

	// Frame `frame`'s jitter: the Halton (2, 3) point of index (frame mod TaaJitterSequenceLength) + 1, minus 0.5 on each axis,
	// in scene pixels (+x right, +y down), so it lies in (-0.5, 0.5). Pure; exact (the Halton points are sums of powers of
	// two and three in float, computed in integer arithmetic then divided once).
	[[nodiscard]] glm::vec2 GetTaaJitter(uint32_t frame);

	// Whether a capture of `snapshot` renders the warm-up: TAA or auto-exposure, in a Lit view with a camera outside the
	// wireframe and pixel-art mode (which ignore both, as the renderer's temporal passes do; pixel-art mode is M18's 2D colour
	// path: RenderOutputSettings::PixelArt). Pure.
	[[nodiscard]] bool NeedsTemporalWarmup(const RenderSnapshot& snapshot);

	class RenderViewHistory
	{
	public:
		// Fills `snapshot`'s temporal inputs and records it (see the file comment). `deltaTime`: seconds since the view's
		// previous render (finite, >= 0; the host's frame time, or 0 for captures and lockstep frames that show the same tick).
		void Apply(RenderSnapshot& snapshot, float deltaTime);

		// The next Apply resets the snapshot's history (a scene opened, closed or swapped, play started or stopped, a capture).
		void Reset();

		// The Applies since the last reset (the jitter's frame), 0 before the first.
		[[nodiscard]] uint32_t GetFrame() const { return m_Frame; }
	private:
		std::optional<CameraData> m_Camera{};
		std::vector<std::pair<UUID, glm::mat4>> m_Worlds{}; // the previous Apply's mesh items, sorted by Entity (stable)
		// M33 (decision 6): the previous Apply's particles, sorted by (emitter entity, incarnation, serial), each with how it was
		// drawn (56 bytes a particle).
		struct ParticleRecord
		{
			UUID Emitter{};
			uint64_t Incarnation = 0;
			uint64_t Serial = 0;
			ParticleDrawState Draw{};
		};
		std::vector<ParticleRecord> m_Particles{};
		uint32_t m_Frame = 0;
		bool m_ResetPending = true;
	};

}
