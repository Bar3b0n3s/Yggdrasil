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

		// Whether `handle` names data of type T that loads through `assets`: success, or why not (InvalidArgument for the null
		// handle, the load's error, Validation for data of another type).
		template<AssetDataType T>
		[[nodiscard]] static Status LoadAs(AssetManager& assets, AssetHandle handle)
		{
			if (!handle.IsValid())
				return MakeError(ErrorCode::InvalidArgument, "no data is assigned");
			const Result<AssetRef<Asset>> loaded = assets.Load(handle);
			if (!loaded.has_value())
				return std::unexpected(loaded.error());
			if (AssetCast<T>(*loaded) == nullptr)
				return MakeError(ErrorCode::Validation, "{} is not {}", handle.ToString(), AssetTypeToString(T::StaticType));
			return {};
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

		// World Tree brief 14 (§7.4): a ReflectionProbe's baked environment that an editor without a graphics device cannot load
		// (`error`, Unsupported: its GPU bake is deferred to an editor with a GPU, EditorAssetManager) is baked already; it
		// reflects nothing in this editor only, so the issue names the deferral and the load's own hint (start the editor with
		// a GPU once), never a lighting bake, which cannot run here and is not the fix.
		[[nodiscard]] static ProbeSceneIssue MakeProbeBakeDeferred(ConstEntity entity, AssetHandle data, const Error& error)
		{
			ProbeSceneIssue issue = MakeProbeNotBaked(entity, "ReflectionProbe", data);
			issue.Message = std::format("the ReflectionProbe of '{}' was baked into {}, whose environment bake is deferred to an editor with a GPU: "
										"this editor has no graphics device, so it reflects nothing here ({})",
				entity.GetName(), data.ToString(), error.GetMessageText());
			issue.Hint = error.GetHint();
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
				if (!Utils::LoadAs<IrradianceVolumeData>(assets, volume->Data.GetHandle()).has_value())
					issues.push_back(Utils::MakeProbeNotBaked(entity, "IrradianceVolume", volume->Data.GetHandle()));
			}
			if (const ReflectionProbeComponent* probe = entity.TryGetComponent<ReflectionProbeComponent>())
			{
				const AssetHandle data = probe->Data.GetHandle();
				if (const Status loaded = Utils::LoadAs<EnvironmentData>(assets, data); !loaded.has_value())
				{
					issues.push_back(loaded.error().GetCode() == ErrorCode::Unsupported ? Utils::MakeProbeBakeDeferred(entity, data, loaded.error())
																						: Utils::MakeProbeNotBaked(entity, "ReflectionProbe", data));
				}
			}
		});
		return issues;
	}

}
