#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/UUID.h"
#include "Engine/Reflection/ValidationContext.h"

#include <array>
#include <string>
#include <string_view>
#include <vector>

// The sky checks of one scene (M45, Docs/Decisions/0063-m45-sky-and-time-of-day.md decision 3): what project.validate reports
// about a scene's Sky (PROBE_NOT_RELIGHTABLE, a probe's, is checked with the probes, Scene/ProbeValidation.h). The codes are
// appended to both of ProjectValidator.cpp's code tables after M44's (ADR 0060's phase-8 ledger) and checked by CheckSky after
// M43's CheckStaticScenery, each with its Tests/Data/Validation/<CODE>/ fixture and its row in the game-debugging skill's
// validation-codes page; the renderer draws such a sky as the code says without reporting it again.

namespace Engine {

	class Scene;

	// Warning, not fixable: an effectively enabled Sky whose Sun names an entity without a DirectionalLight, or a disabled one
	// (the sky then has no sun: a dark sky, nothing driven, and the glow and the cascades go to the other directional lights),
	// whose Sun is null in a scene without an enabled DirectionalLight, or whose Moon is its Sun's entity (no moon is drawn). A
	// Sun or Moon that names no entity is ENTITY_DANGLING_REFERENCE's, never this code's.
	inline constexpr std::string_view SkyLightInvalidCode = "SKY_LIGHT_INVALID";
	// Warning, not fixable: an effectively enabled Environment beside an effectively enabled Sky: the Sky lights the scene and
	// draws its sky, and the Environment is unused.
	inline constexpr std::string_view SkyEnvironmentIgnoredCode = "SKY_ENVIRONMENT_IGNORED";
	// Warning, not fixable (W45.3; checked by Scene/ProbeValidation.h's FindProbeSceneIssues): under an effectively enabled
	// Sky, an IrradianceVolume whose data loads but has no relight block (baked without a Sky: it keeps the bake's light as the
	// sky turns), or a ReflectionProbe whose data loads and whose centre lies in no relightable volume's box (it keeps its
	// bake's brightness). The fix is a bake under the Sky (lighting.bake).
	inline constexpr std::string_view ProbeNotRelightableCode = "PROBE_NOT_RELIGHTABLE";
	// The sky codes, an array of their own (ADR 0060's phase-8 ledger; M42's pattern).
	inline constexpr std::array<std::string_view, 3> SkyDiagnosticCodes = { SkyLightInvalidCode, SkyEnvironmentIgnoredCode, ProbeNotRelightableCode };

	// One sky validation finding.
	struct SkySceneIssue
	{
		std::string Code{}; // SkyLightInvalidCode or SkyEnvironmentIgnoredCode
		DiagnosticSeverity Severity = DiagnosticSeverity::Warning;
		UUID Entity{};           // the Sky's entity, or the Environment's
		std::string Component{}; // "Sky" or "Environment"
		std::string Field{};     // "Sun", "Moon" or empty
		std::string Message{};
		std::string Hint{};

		bool operator==(const SkySceneIssue&) const = default;
	};

	// The sky checks over `scene`, in canonical order (effectively enabled entities): for the first effectively enabled Sky,
	// SKY_LIGHT_INVALID on its Sun (Field "Sun") and then on its Moon (Field "Moon"), at most one each; then
	// SKY_ENVIRONMENT_IGNORED on each effectively enabled Environment when the scene has an effectively enabled Sky. A scene
	// without a Sky gives none. Pure function of the scene; main thread.
	[[nodiscard]] std::vector<SkySceneIssue> FindSkyIssues(const Scene& scene);

}
