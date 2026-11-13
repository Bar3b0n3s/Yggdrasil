#include "EnginePCH.h"
#include "Engine/Scene/ProbeValidation.h"

#include "Engine/Asset/AssetManager.h"
#include "Engine/Asset/EnvironmentData.h"
#include "Engine/Asset/IrradianceVolumeData.h"
#include "Engine/Scene/Components/IrradianceVolumeComponent.h"
#include "Engine/Scene/Components/ReflectionProbeComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"

#include <format>
#include <utility>

// The probe checks of a scene (ProbeValidation.h; Docs/Decisions/0037-m26-contract.md decision 9).

namespace Engine {

	namespace Utils {

		// Whether `handle` names data of type T that loads through `assets`.
		template<AssetDataType T>
		[[nodiscard]] static bool LoadsAs(AssetManager& assets, AssetHandle handle)
		{
			if (!handle.IsValid())
				return false;
			const Result<AssetRef<Asset>> loaded = assets.Load(handle);
			return loaded.has_value() && AssetCast<T>(*loaded) != nullptr;
		}

		[[nodiscard]] static ProbeSceneIssue MakeProbeNotBaked(ConstEntity entity, std::string_view component, AssetHandle data)
		{
			ProbeSceneIssue issue;
			issue.Code = std::string(ProbeNotBakedCode);
			issue.Severity = DiagnosticSeverity::Warning;
			issue.Entity = entity.GetUUID();
			issue.Component = std::string(component);
			issue.Field = "Data";
			issue.Message = data.IsValid()
				? std::format("the {} of '{}' was baked into {}, which does not load: it lights nothing", component, entity.GetName(), data.ToString())
				: std::format("the {} of '{}' is not baked: it lights nothing", component, entity.GetName());
			issue.Hint = "Bake the scene's lighting with lighting.bake (the editor's Bake Lighting) and save the scene.";
			return issue;
		}

	}

	std::vector<ProbeSceneIssue> FindProbeSceneIssues(const Scene& scene, AssetManager& assets)
	{
		std::vector<ProbeSceneIssue> issues;
		if (scene.GetRegistry().view<IrradianceVolumeComponent>().empty() && scene.GetRegistry().view<ReflectionProbeComponent>().empty())
			return issues;
		scene.ForEachCanonical([&issues, &assets](ConstEntity entity)
		{
			if (!entity.IsActive())
				return;
			if (const IrradianceVolumeComponent* volume = entity.TryGetComponent<IrradianceVolumeComponent>())
			{
				if (!Utils::LoadsAs<IrradianceVolumeData>(assets, volume->Data.GetHandle()))
					issues.push_back(Utils::MakeProbeNotBaked(entity, "IrradianceVolume", volume->Data.GetHandle()));
			}
			if (const ReflectionProbeComponent* probe = entity.TryGetComponent<ReflectionProbeComponent>())
			{
				if (!Utils::LoadsAs<EnvironmentData>(assets, probe->Data.GetHandle()))
					issues.push_back(Utils::MakeProbeNotBaked(entity, "ReflectionProbe", probe->Data.GetHandle()));
			}
		});
		return issues;
	}

}
