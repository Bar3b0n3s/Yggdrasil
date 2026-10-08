#pragma once

#include "Engine/Core/Base.h"

// When a host's per-frame collections of stale GPU mirrors also release what the last frame did not use (SceneRenderer.h,
// "Stale mirrors"; §8.14 items 1 and 2; Docs/Decisions/0013-m8-decisions.md decision 7): GpuResourceCache::CollectStale and
// SceneRendererPipelines::CollectStale run once per frame before the frame's renders, and pass releaseUnused = true exactly
// once after the scene a host shows changed, at the first collection after a frame rendered the new scene. Releasing
// earlier would drop the mirrors the new scene is about to use again; releasing every frame would re-upload whatever a
// frame happened not to draw.
//
// Both hosts drive one schedule per view they keep mirrors for: EditorApp calls NoteSceneChange when ShownSceneTracker
// (EditorCore) notices a scene opened, closed or swapped, play mode included, and RuntimeApp once for its start scene;
// each frame then takes TakeReleaseUnused for its collections and calls NoteRendered after its render. Pure state, no GPU;
// main thread only.

namespace Engine {

	class StaleMirrorSchedule
	{
	public:
		// The shown scene changed: the next release waits for a frame rendered after this call (a render noted before it
		// showed the previous scene).
		void NoteSceneChange();

		// A frame rendered the shown scene. No effect while no scene change waits for its release.
		void NoteRendered();

		// The releaseUnused argument of this frame's collections: true exactly once after NoteSceneChange was followed by
		// NoteRendered (which ends the wait), false otherwise.
		[[nodiscard]] bool TakeReleaseUnused();

		// Whether a scene change still waits for its release.
		[[nodiscard]] bool IsReleasePending() const;
	private:
		bool m_IsReleasePending = false; // a scene change awaits its release
		bool m_HasRendered = false;      // a frame rendered since that change
	};

}
