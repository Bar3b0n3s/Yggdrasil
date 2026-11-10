#pragma once

#include "Engine/Automation/Methods/AutomationTypes.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"

#include <string>

// animation.preview (M23, C23.3; Docs/Decisions/0034-m23-contract.md decision 32): the editor's animation preview
// (EditorCore/Animation/AnimationPreview.h) as a method, the one action of the preview panel's controls, so automation
// previews exactly as the editor does (Roadmap rule 5). It poses an open edit scene's Animator without changing the scene:
// nothing is written, no undo step is made and the scene's revision stays, so it is not Mutating and not a batch op. It
// addresses an entity of an open scene, so it takes M20's scene target (SupportsSceneTarget; the scene-target inventory's
// relation "the target scene's entity"), and refuses the play scene (a play session poses its Animators itself).
//
// Frozen by the M23 contract C23.3: the wire format below. The types and the method are written here; stream D implements
// the handler and calls the registration functions from RegisterMethods.cpp, together with its C++ round trip, its Python
// test, its scene-target inventory entry and the regenerated references and catalogue.

namespace Engine {

	class EditorMethodContext;
	class MethodRegistry;
	class TypeRegistry;

	// animation.preview {entity, time?, playing?, stop?} (and "scene", the reserved scene target: the open edit scene the
	// preview poses, the active one without it). An absent time or playing keeps the running preview's when it previews the
	// same scene and entity (MethodContext::HasParam), 0 and false for a new one.
	struct AnimationPreviewParams
	{
		std::string Entity{}; // required: an entity of the target scene holding an Animator (an EntityRef)
		float Time = 0.0f;    // seconds added to every layer's time times its Speed, in [0, MaxAnimatorTime]
		bool Playing = false; // whether the editor's frames advance the time
		bool Stop = false;    // end the preview: the entity draws its bind pose again; time and playing are then refused
	};

	struct AnimationPreviewResult
	{
		EntitySummary Entity{};
		bool Previewing = false; // false after stop
		float Time = 0.0f;
		bool Playing = false;
		float Duration = 0.0f; // the longest loaded layer clip's duration, in seconds
	};

	namespace Automation {

		// animation.preview: AnimationPreview::Begin in the target scene with the params over the preview's state (a preview
		// running in another scene or of another entity ends first, its pose removed from its own scene), or with stop End when
		// the running preview is the target scene's `entity` (otherwise stop changes nothing). Errors: NotFound at /entity for
		// an entity the target scene does not hold (ResolveEntity's), InvalidArgument at /entity for one without an Animator or
		// for a malformed or ambiguous reference, at /time for a time that is not finite or outside [0, MaxAnimatorTime] (the
		// registered field's range), and at /time or /playing with stop; InvalidState for the play scene as the target (the
		// hint names play.stop); NotFound naming the Skeleton when it is null or fails to load (no preview is then started, and
		// no pose written).
		[[nodiscard]] Result<AnimationPreviewResult> AnimationPreviewMethod(EditorMethodContext& context, const AnimationPreviewParams& params);

	}

	// Registers the structs above.
	void RegisterAnimationMethodTypes(TypeRegistry& registry);

	// Registers animation.preview: not mutating, no dry run, not a batch op, SupportsSceneTarget, an MCP tool; not
	// available in the Runtime.
	void RegisterAnimationMethods(MethodRegistry& methods);

}
