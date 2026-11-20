#include "EditorPCH.h"
#include "EditorCore/Automation/LightingMethods.h"

#include "EditorCore/Automation/EditorMethodContext.h"
#include "EditorCore/EditorContext.h"
#include "EditorCore/Lighting/LightingBake.h"
#include "Engine/Automation/Protocol/MethodRegistry.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"

#include <nlohmann/json.hpp>

#include <span>
#include <utility>
#include <vector>

// lighting.bake (Docs/Decisions/0037-m26-contract.md decision 8): BakeLighting on the request's scene, its entries as the
// method's summaries.

namespace Engine {

	namespace Utils {

		// The method's entries of `entries`, baked as `type` assets in the addressed scene.
		[[nodiscard]] static std::vector<LightingBakeProbe> SummarizeBakedProbes(EditorMethodContext& context, std::span<const LightingBakeEntry> entries,
			AssetType type)
		{
			const Scene& scene = context.GetEditor().GetScene();
			std::vector<LightingBakeProbe> probes;
			probes.reserve(entries.size());
			for (const LightingBakeEntry& entry : entries)
			{
				probes.push_back({ .Entity = context.MakeEntitySummary(scene.FindEntityByID(entry.Entity)),
					.Path = entry.Path,
					.Asset = AssetSummary{ .Id = entry.Asset.ToString(), .Path = entry.Path, .Type = type },
					.Changed = entry.Changed });
			}
			return probes;
		}

	}

	namespace Automation {

		Result<LightingBakeMethodResult> LightingBakeMethod(EditorMethodContext& context, const LightingBakeParams& params)
		{
			ENGINE_TRY_ASSIGN(const OpenSceneId scene, context.ResolveSceneTarget());
			ENGINE_TRY_ASSIGN(const LightingBakeResult baked,
				BakeLighting(context.GetEditor(), scene, { .IrradianceVolumes = params.IrradianceVolumes, .ReflectionProbes = params.ReflectionProbes }));
			LightingBakeMethodResult result;
			result.IrradianceVolumes = Utils::SummarizeBakedProbes(context, baked.IrradianceVolumes, AssetType::IrradianceVolume);
			result.ReflectionProbes = Utils::SummarizeBakedProbes(context, baked.ReflectionProbes, AssetType::Environment);
			return result;
		}

	}

	void RegisterLightingMethodTypes(TypeRegistry& registry)
	{
		registry.Struct<LightingBakeParams>("LightingBakeParams", "The params of lighting.bake.")
			.Field("irradianceVolumes", &LightingBakeParams::IrradianceVolumes, "Whether to bake the scene's IrradianceVolumes (default true).")
			.Field("reflectionProbes", &LightingBakeParams::ReflectionProbes, "Whether to bake the scene's ReflectionProbes (default true).");
		registry.Struct<LightingBakeProbe>("LightingBakeProbe", "One probe lighting.bake baked.")
			.Field("entity", &LightingBakeProbe::Entity, "The probe's entity.")
			.Field("path", &LightingBakeProbe::Path, "The written source file, project-relative: Assets/Lighting/<the scene's path below Assets/ without .scene>/<entity id>.irradiance or .hdr.")
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
				.Description = "Bakes an edit scene's IrradianceVolumes and ReflectionProbes: renders their surroundings, writes their data for "
							   "the saved scene (Assets/Lighting/<the scene's path below Assets/ without .scene>/, so Assets/Scenes/Room.scene "
							   "bakes into Assets/Lighting/Scenes/Room/), imports it and sets each probe's Data in one undoable edit. Two bakes "
							   "of one scene write byte-identical files. Each capture and import frees what it used before the next, so a bake's "
							   "device allocations stay the same whatever its probe count, and baking again adds none.",
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
