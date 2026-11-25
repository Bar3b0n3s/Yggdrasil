#pragma once

#include "Engine/Automation/Methods/AutomationTypes.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"

#include <string>
#include <vector>

// lighting.bake (M26, C26.1, R-GI-PROBE; Docs/Decisions/0037-m26-contract.md decision 8): the editor's Bake Lighting action
// as a method (EditorCore/Lighting/LightingBake.h's BakeLighting, the action's one call), so automation bakes exactly as the
// editor does (Roadmap rule 5). It writes files and changes the scene's probes through one undoable edit, so it Mutates; it
// writes files directly, so it is no batch op and takes no dry run. It addresses an open edit scene, so it takes M20's scene
// target (SupportsSceneTarget; the scene-target inventory's relation "the target scene's probes"), and refuses the play
// scene. A bake renders many captures, so the bridge waits up to 600 s.
//
// Frozen by the M26 contract C26.1: the wire format below. The types and the method are written here; stream Q-A implements
// the handler and calls the registration functions from RegisterMethods.cpp, together with its C++ round trip, its Python
// test (Tests/Automation/test_lighting.py), its scene-target inventory entry and the regenerated references and catalogue.
// M45 (Docs/Decisions/0063-m45-sky-and-time-of-day.md decision 3, a reviewed change of the frozen format) appends
// LightingBakeProbe's relightable.

namespace Engine {

	class EditorMethodContext;
	class MethodRegistry;
	class TypeRegistry;

	// lighting.bake {irradianceVolumes?, reflectionProbes?} (and "scene", the reserved scene target: the open edit scene to
	// bake, the active one without it).
	struct LightingBakeParams
	{
		bool IrradianceVolumes = true; // bake the scene's IrradianceVolumes
		bool ReflectionProbes = true;  // bake the scene's ReflectionProbes
	};

	// One baked probe of the result.
	struct LightingBakeProbe
	{
		EntitySummary Entity{};
		std::string Path{}; // the written source, project-relative
		AssetSummary Asset{};
		bool Changed = false;     // whether the file's bytes changed
		bool Relightable = false; // M45: an IrradianceVolume baked under an enabled Sky (relit by the sky at run time)
	};

	struct LightingBakeMethodResult
	{
		std::vector<LightingBakeProbe> IrradianceVolumes{};
		std::vector<LightingBakeProbe> ReflectionProbes{};
	};

	namespace Automation {

		// lighting.bake: BakeLighting on the target scene. Errors: those of BakeLighting (InvalidState for an unsaved scene, the
		// play scene or an editor without a renderer, with their hints; Validation for a probe out of its ranges, before anything
		// renders; Gpu, Io and ImportFailed from the bake).
		[[nodiscard]] Result<LightingBakeMethodResult> LightingBakeMethod(EditorMethodContext& context, const LightingBakeParams& params);

	}

	// Registers the structs above.
	void RegisterLightingMethodTypes(TypeRegistry& registry);

	// Registers lighting.bake: mutating, no dry run, not a batch op, SupportsSceneTarget, an MCP tool with a 600 s timeout;
	// not available in the Runtime.
	void RegisterLightingMethods(MethodRegistry& methods);

}
