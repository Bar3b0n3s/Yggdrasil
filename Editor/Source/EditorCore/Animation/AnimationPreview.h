#pragma once

#include "EditorCore/OpenSceneId.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Core/UUID.h"

#include <optional>

// The editor's animation preview (M23, C23.3; Docs/Decisions/0034-m23-contract.md decision 32): an edit scene's Animator
// shown posed without playing the scene. Edit scenes draw skinned meshes in their bind pose; while a preview runs, its
// Animator's entity holds an AnimationPoseComponent (a runtime-only component, so the scene's revision, its change tracker,
// its undo history and its file never change) sampled at each layer's Time advanced by the preview's time times its Speed
// (Scene/AnimationSystem.h's PoseAnimator), so the scene and game views, thumbnails and viewport.screenshot of the edit
// scene show it. While Playing, the editor's frame advances the preview's time by its wall-clock frame time (presentation
// only: the scene's simulation state is untouched, and automation sets an exact time through animation.preview, the
// preview's one action, EditorCore/Automation/AnimationMethods.h).
//
// One preview at a time, and it belongs to one open scene (M20's several open edit scenes, EditorContext): the preview
// records the OpenSceneId of the scene it poses, and every member resolves that scene through the EditorContext, so a pose
// is only ever written and removed in the scene that holds it. The preview ends, removing its pose from its scene (the bind
// pose again), when it is ended, when a preview of another entity or another scene begins, when the user switches the
// active tab (the frame's Update sees another active scene than the one active when the preview began), when its entity is
// destroyed or loses its Animator; when its scene closes or is replaced (EditorContext::FindScene no longer finds it) the
// pose goes with that scene and the preview ends at the next frame. A scene saved as a copy shares its entities' UUIDs, so
// the preview compares the scene and the entity, never the entity alone.
//
// The UI lives in a panel file of its own (Editor/Panels/AnimationPreviewPanel), which EditorLayer only calls, and the
// editor's frame calls Update from its own file, so M21's single-owner editor files gain one call each (ADR 0028's phase-2
// ledger). Main thread, like the scene. Frozen by the M23 contract C23.3; stream D implements it.

namespace Engine {

	class AssetManager;
	class EditorContext;

	// What a running preview shows.
	struct AnimationPreviewState
	{
		OpenSceneId Scene = NoOpenScene;       // the open edit scene the preview poses (EditorContext's id)
		OpenSceneId ActiveScene = NoOpenScene; // the active scene when the preview began: Update ends it at a tab switch
		UUID Entity{};                         // the previewed Animator's entity, in Scene
		float Time = 0.0f;                     // seconds added to every layer's time (times its Speed), >= 0
		bool Playing = false;                  // Update advances Time by the frame's time
		float Duration = 0.0f;                 // the longest loaded layer clip's Duration (0 without one): the slider's end

		bool operator==(const AnimationPreviewState&) const = default;
	};

	class AnimationPreview
	{
	public:
		AnimationPreview() = default;
		~AnimationPreview() = default;

		AnimationPreview(const AnimationPreview&) = delete;
		AnimationPreview& operator=(const AnimationPreview&) = delete;

		// Starts (or changes) the preview of the Animator on `entity` of the open edit scene `scene` of `context` at `time`
		// seconds, playing or not, and poses it at once (PoseAnimator, its assets from `assets`), so a capture right after the
		// call shows that pose. A running preview of another entity or another scene ends first, its pose removed from its own
		// scene; the same scene and entity changes the running preview. Records the active scene of `context` (ActiveScene),
		// against which Update detects a tab switch. `context` and `assets` are borrowed for the call. Errors: NotFound when no
		// open scene has the id `scene` or that scene has no entity `entity`, InvalidArgument when the entity holds no Animator,
		// for a time that is not finite or below 0, and PoseAnimator's errors (the preview is then not started).
		[[nodiscard]] Result<AnimationPreviewState> Begin(EditorContext& context, OpenSceneId scene, UUID entity, AssetManager& assets,
			float time, bool playing);

		// The editor's frame: ends the preview, removing its pose where its scene is still open, when the active scene of
		// `context` differs from the one active when it began (the user switched tabs), when its scene is no longer open, or
		// when its entity is gone or holds no Animator; otherwise, while Playing, advances Time by `frameSeconds` (>= 0,
		// finite), wrapping at Duration when every layer loops, and poses the Animator again (its fields may have changed).
		// Does nothing without a preview.
		void Update(EditorContext& context, AssetManager& assets, double frameSeconds);

		// Ends the preview: removes the entity's AnimationPoseComponent from the preview's scene when that scene is still open
		// (it draws its bind pose again). Does nothing without a preview.
		void End(EditorContext& context);

		// The running preview, or nullopt.
		[[nodiscard]] const std::optional<AnimationPreviewState>& GetState() const { return m_State; }
	private:
		std::optional<AnimationPreviewState> m_State{};
	};

}
