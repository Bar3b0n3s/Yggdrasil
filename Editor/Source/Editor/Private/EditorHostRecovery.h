#pragma once

#include "Editor/EditorPanelContext.h"
#include "EditorCore/OpenSceneId.h"
#include "Engine/Core/Result.h"

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace Engine {

	class Autosave;
	class EditorContext;
	class LoadedProject;

	// Main-thread recovery offer/decision owner shared by EditorApp and its panel tests. The referenced editor and
	// autosave service outlive this helper. No UI code or callback performs disk reads or recovery directly.
	class EditorHostRecovery
	{
	public:
		EditorHostRecovery(EditorContext& editor, Autosave& saves);
		// Inspect once per nonzero projectEpoch, supplied by the host after a successful open and incremented at each
		// subsequent open. A repeated inspection of the same project/epoch preserves dismissal and pending/failure state.
		// A new epoch invalidates the previous offer. Already-dirty/recovered scenes are not offered again: no offer while
		// any open scene is dirty. Propagates FindRecoveries errors without changing source/scene/history; InvalidState
		// without an open project. M20 (Docs/Decisions/0031-m20-contract.md decision 15): one offer covers every scene with a
		// recoverable generation (Autosave::FindRecoveries); its Recovery names the newest of them.
		[[nodiscard]] Status Inspect(uint64_t projectEpoch);
		// Owned, memory-only copy for the UI. A failed queued decision remains here with Failure set and Pending false.
		[[nodiscard]] std::optional<EditorRecoveryOffer> GetOffer() const;
		// Memory-only enqueue. InvalidArgument for zero ID/unknown decision; NotFound for an obsolete offer;
		// InvalidState if already pending. Accept returns Conflict for changed epoch/project/revision, InvalidState while
		// a test run owns the editor and PermissionDenied for read-only acceptance. Decline of the matching offer remains
		// valid after binding changes. M20 (decision 15): the revision is every open scene's, with the open scenes
		// themselves, since Accept replaces any open scene the offer names, the active one or not.
		// Only the host-owned offer bytes are used; caller-provided Recovery/Failure fields cannot replace them.
		// Binding/read-only refusals remain in the matching offer's Failure. Bad/obsolete requests never change it;
		// an already-pending refusal preserves the original queued decision, and a successful enqueue clears Failure.
		[[nodiscard]] Status QueueDecision(const EditorRecoveryOffer& offer, EditorRecoveryDecision decision);
		// Consume at the next safe point, rechecking epoch/project/revision and test-run ownership before any recovery. Accept uses the real
		// Autosave::Recover on the existing lock/mounts, for every offered scene in FindRecoveries' order, then makes the first
		// of them active; a failure stops at that scene, the ones before it staying recovered. Decline only dismisses the
		// matching offer in memory, even when
		// its binding became stale. Decline never reads/writes current or durable state. Failure stays observable in
		// GetOffer; success dismisses. With no pending decision, succeeds without I/O or state changes.
		[[nodiscard]] Status Pump();
		// Clear offer/decision/bindings before project unlock; never reset the monotonically increasing offer ID counter.
		void Reset();
	private:
		[[nodiscard]] Status CheckBinding() const;
	private:
		EditorContext& m_Editor;
		Autosave& m_Saves;
		const LoadedProject* m_Project = nullptr; // identity-only back-reference, never dereferenced; Reset before close
		uint64_t m_ProjectEpoch = 0;
		uint64_t m_OfferEpoch = 0;
		std::vector<std::pair<OpenSceneId, uint64_t>> m_Revisions{}; // every open scene's revision at Inspect, in tab order
		uint64_t m_NextOffer = 0;
		std::optional<EditorRecoveryOffer> m_Offer{};
		std::vector<AutosaveRecoveryInfo> m_Recoveries{}; // every scene the offer covers (FindRecoveries' order)
		std::optional<EditorRecoveryDecision> m_Decision{};
	};

}
