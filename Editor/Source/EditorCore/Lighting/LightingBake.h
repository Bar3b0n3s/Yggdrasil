#pragma once

#include "EditorCore/OpenSceneId.h"
#include "Engine/Asset/AssetHandle.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Core/UUID.h"

#include <string>
#include <vector>

// lighting.bake's work (M26, C26.1, R-GI-PROBE; Docs/Decisions/0037-m26-contract.md decision 8): bakes an open edit scene's
// probes and commits their data beside the scene. The editor's Bake Lighting action (a menu item in a file of its own,
// Editor/Panels/LightingBakeAction, that EditorLayer only calls; ADR 0028's phase-3 ledger) and the method call this one
// function, so automation bakes exactly as the editor does (Roadmap rule 5).
//   - The scene must be saved (its file names the bake's folder): Assets/Lighting/ followed by the scene's project path below
//     Assets/ without its extension (Assets/Scenes/Room.scene bakes into Assets/Lighting/Scenes/Room/), so two scenes of one
//     name in different folders never share a file, even when one is a copy that kept the other's entity ids (W26.1's
//     review, Docs/Decisions/0037-m26-contract.md decision 31).
//   - Every effectively enabled IrradianceVolume and ReflectionProbe of the scene, in canonical entity order (each kind
//     unless its option is off), is checked first: a volume's counts and Size within IrradianceVolumeData's rules and a
//     reflection probe's Resolution a power of two in [MinReflectionProbeResolution, MaxReflectionProbeResolution], so a
//     probe out of its ranges (COMPONENT_FIELD_OUT_OF_RANGE) refuses the bake before anything renders or is written. The
//     scene's render snapshot is then extracted once (the editor's scene camera never matters: captures place their own),
//     and for an IrradianceVolume one capture per probe of its grid (16 texels a face) projected
//     to SH9 (Renderer/ProbeCapture.h), written as `<entity id>.irradiance` (WriteIrradianceVolumeSource); for a
//     ReflectionProbe one capture of its Resolution at its position, written as `<entity id>.hdr` (WriteRadianceHdr) with a
//     `.meta` whose EnvironmentImportSettings are the defaults. Files are named by entity id (16 hexadecimal digits), so a
//     renamed entity keeps its data and two entities never share a file.
//   - The written files are imported (their handles kept across bakes: a rewritten source keeps its .meta), and every
//     probe's Data is set to its asset in one undoable edit of the scene ("Bake Lighting"); a probe whose data did not change
//     keeps its handle, so a second bake changes no component and writes byte-identical files (the acceptance). A bake that
//     changes no component records no undo step (an EditorTransaction without a command records nothing), so an undo after
//     it takes back the edit before it, such as the previous bake's assignment.
//   - Captures render without the probes (RenderQualitySettings::Probes false): one bounce.
//   - World Tree's brief 21: every capture collects the garbage of its submissions before it returns (ProbeCapture), and so
//     does a reflection probe's import, which bakes its environment on the GPU (EnvironmentBaker), both through
//     GraphicsDevice::CollectCompletedSubmissions, so a bake holds one capture's and one import's device allocations whatever
//     its probe count, and a bake straight after another adds none.
// Main thread (GPU work and the scene). Frozen by the M26 contract C26.1; stream Q-A implements it.

namespace Engine {

	class EditorContext;

	struct LightingBakeOptions
	{
		bool IrradianceVolumes = true;
		bool ReflectionProbes = true;
	};

	// One baked probe.
	struct LightingBakeEntry
	{
		UUID Entity{};
		std::string Path{}; // the written source, project-relative ("Assets/Lighting/Scenes/Room/7a1c3e5f20b4d608.irradiance")
		AssetHandle Asset{};
		bool Changed = false; // whether the file's bytes changed (false for a rebake of an unchanged scene)
	};

	struct LightingBakeResult
	{
		std::vector<LightingBakeEntry> IrradianceVolumes{}; // canonical entity order
		std::vector<LightingBakeEntry> ReflectionProbes{};  // canonical entity order
	};

	// Bakes the open edit scene `scene` (see the file comment). Errors: InvalidState for a scene never saved (the hint names
	// scene.save), for the play scene, and without a GPU device (headless with --renderer none: the hint names a rendering
	// editor); NotFound for an unknown open scene; Validation for a probe out of its ranges, naming its entity, checked before
	// the GPU device, so nothing renders or is written; those of the capture (Gpu), the writes (Io) and the imports
	// (ImportFailed), after which the files already written stay and no component changes.
	[[nodiscard]] Result<LightingBakeResult> BakeLighting(EditorContext& editor, OpenSceneId scene, const LightingBakeOptions& options);

}
