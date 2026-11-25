#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/UUID.h"
#include "Engine/Reflection/ValidationContext.h"

#include <string>
#include <string_view>
#include <vector>

// The probe checks of one scene (M26, C26.1, R-GI-PROBE; Docs/Decisions/0037-m26-contract.md decision 9): what project.validate
// reports about a scene's IrradianceVolumes and ReflectionProbes. The code is appended after main's in both of
// ProjectValidator.cpp's code tables in landing order (ADR 0028's phase-3 ledger: M26's after M24's at its merge), with its
// Tests/Data/Validation/PROBE_NOT_BAKED/ fixture and its row in the game-debugging skill's validation-codes page; the renderer
// behaves as the code says (a probe without data lights and reflects nothing, RenderSnapshot.h's IrradianceVolumeItem and
// ReflectionProbeItem).

namespace Engine {

	class AssetManager;
	class Scene;

	// Warning, not fixable: an IrradianceVolume or ReflectionProbe has no Data, or its Data does not load; it lights nothing.
	inline constexpr std::string_view ProbeNotBakedCode = "PROBE_NOT_BAKED";

	// One probe validation finding.
	struct ProbeSceneIssue
	{
		std::string Code{}; // ProbeNotBakedCode, or M45's ProbeNotRelightableCode (Scene/SkyValidation.h)
		DiagnosticSeverity Severity = DiagnosticSeverity::Warning;
		UUID Entity{};           // the probe's entity
		std::string Component{}; // "IrradianceVolume" or "ReflectionProbe"
		std::string Field{};     // "Data"
		std::string Message{};
		std::string Hint{};

		bool operator==(const ProbeSceneIssue&) const = default;
	};

	// The probe checks over `scene`: every effectively enabled entity's IrradianceVolume, then its ReflectionProbe, in
	// canonical entity order, whose Data is null or does not load through `assets` (a missing asset, a failed import or
	// another type) is PROBE_NOT_BAKED, with the hint to bake the scene's lighting. World Tree brief 14 (§7.4): a
	// ReflectionProbe whose Data load is Unsupported, an environment whose GPU bake an editor without a graphics device
	// deferred, is baked already, so its issue says the bake is deferred to an editor with a GPU and carries the load's hint
	// (start the editor with a GPU once) instead. M45 (Docs/Decisions/0063-m45-sky-and-time-of-day.md decision 3): in a scene
	// with an effectively enabled Sky, an IrradianceVolume whose data loads without a relight block, and a ReflectionProbe whose
	// data loads and whose centre (its world position) lies in the box of no effectively enabled IrradianceVolume whose data loads
	// with one, is PROBE_NOT_RELIGHTABLE (Scene/SkyValidation.h's ProbeNotRelightableCode) on its Data, in the same order, with
	// the hint to bake under the Sky. Loads the probes' data (the editor's asset manager imports what it has not); main thread.
	[[nodiscard]] std::vector<ProbeSceneIssue> FindProbeSceneIssues(const Scene& scene, AssetManager& assets);

}
