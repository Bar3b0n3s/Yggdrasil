#include "EditorPCH.h"
#include "EditorCore/Automation/LightingMethods.h"

#include "EditorCore/Automation/EditorMethodContext.h"
#include "Engine/Automation/Protocol/MethodRegistry.h"
#include "Engine/Reflection/TypeRegistry.h"

#include <nlohmann/json.hpp>

// lighting.bake (Docs/Decisions/0037-m26-contract.md decision 8). The wire types and the registration are the contract's,
// written but not called; the handler is a contract stub, implemented by stream Q-A.

namespace Engine {

	namespace Automation {

		Result<LightingBakeMethodResult> LightingBakeMethod(EditorMethodContext& /*context*/, const LightingBakeParams& /*params*/)
		{
			ENGINE_CONTRACT_STUB();
			return MakeError(ErrorCode::Unsupported, "M26 probes contract (Docs/Decisions/0037-m26-contract.md decision 8, stream Q-A)");
		}

	}

	void RegisterLightingMethodTypes(TypeRegistry& registry)
	{
		registry.Struct<LightingBakeParams>("LightingBakeParams", "The params of lighting.bake.")
			.Field("irradianceVolumes", &LightingBakeParams::IrradianceVolumes, "Whether to bake the scene's IrradianceVolumes (default true).")
			.Field("reflectionProbes", &LightingBakeParams::ReflectionProbes, "Whether to bake the scene's ReflectionProbes (default true).");
		registry.Struct<LightingBakeProbe>("LightingBakeProbe", "One probe lighting.bake baked.")
			.Field("entity", &LightingBakeProbe::Entity, "The probe's entity.")
			.Field("path", &LightingBakeProbe::Path, "The written source file, project-relative: Assets/Lighting/<scene>/<entity id>.irradiance or .hdr.")
			.Field("asset", &LightingBakeProbe::Asset, "The baked asset, which the probe's Data now references.")
			.Field("changed", &LightingBakeProbe::Changed, "Whether the file's bytes changed (false when a rebake found the same light).");
		registry.Struct<LightingBakeMethodResult>("LightingBakeMethodResult", "The probes lighting.bake baked, in canonical entity order.")
			.Field("irradianceVolumes", &LightingBakeMethodResult::IrradianceVolumes, "The baked IrradianceVolumes.")
			.Field("reflectionProbes", &LightingBakeMethodResult::ReflectionProbes, "The baked ReflectionProbes.");
	}

	void RegisterLightingMethods(MethodRegistry& methods)
	{
		methods.Add(
			{
				.Name = "lighting.bake",
				.Description = "Bakes an edit scene's IrradianceVolumes and ReflectionProbes: renders their surroundings, writes their data beside "
							   "the saved scene (Assets/Lighting/<scene>/), imports it and sets each probe's Data in one undoable edit. Two bakes "
							   "of one scene write byte-identical files.",
				.ExposeAsTool = true,
				.Mutates = true,
				.SupportsSceneTarget = true,
				.TimeoutSeconds = 600,
				.Examples = { { .Description = "Bake every probe of the active scene.", .Params = Json::object() },
					{ .Description = "Bake only the reflection probes.", .Params = { { "irradianceVolumes", false } } } },
			},
			&Automation::LightingBakeMethod);
	}

}
