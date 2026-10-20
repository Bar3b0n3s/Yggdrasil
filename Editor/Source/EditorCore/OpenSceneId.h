#pragma once

#include <cstddef>
#include <cstdint>

// The identity of the editor's open scenes (M20, E-SCENES; Docs/Decisions/0031-m20-contract.md decision 13), in a header of
// its own so the method contexts and the automation structs that name an open scene need not include EditorContext.h.

namespace Engine {

	// An open scene's identity: assigned when a scene opens (EditorContext::SetScene, AddScene, a recovery) from one counter
	// that starts at 1, and never reused for the editor's lifetime, so an id taken before a scene closed or was replaced never
	// names another one. NoOpenScene names none. It is 32 bits wide everywhere: scene.list, scene.new, scene.open and
	// scene.close report it as a uint32, and a request's scene target (RequestOptions::Scene) accepts it as one, so no result
	// narrows it. The counter never wraps: an editor that has opened 2^32 - 1 scenes fails the next open as a verified
	// programmer error (ENGINE_VERIFY), as the dispatcher's request sequence does.
	using OpenSceneId = uint32_t;
	inline constexpr OpenSceneId NoOpenScene = 0;

	// The most scenes one editor holds open at once (decision 13): autosave's fatal path keeps one fixed snapshot slot per
	// open scene, so the count is bounded.
	inline constexpr size_t MaxOpenScenes = 16;

}
