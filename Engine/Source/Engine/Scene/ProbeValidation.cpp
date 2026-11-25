#include "EnginePCH.h"
#include "Engine/Scene/ProbeValidation.h"

#include "Engine/Asset/AssetManager.h"
#include "Engine/Asset/EnvironmentData.h"
#include "Engine/Asset/IrradianceVolumeData.h"
#include "Engine/Scene/Components/IrradianceVolumeComponent.h"
#include "Engine/Scene/Components/ReflectionProbeComponent.h"
#include "Engine/Scene/Components/SkyComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/SkyValidation.h"
#include "Engine/Scene/TransformSystem.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <format>
#include <utility>
#include <vector>

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

		// M45: whether `scene` has an effectively enabled Sky.
		[[nodiscard]] static bool HasActiveSky(const Scene& scene)
		{
			for (const entt::entity handle : scene.GetRegistry().view<SkyComponent>())
			{
				if (ConstEntity(handle, &scene).IsActive())
					return true;
			}
			return false;
		}

		[[nodiscard]] static ProbeSceneIssue MakeProbeNotRelightable(ConstEntity entity, std::string_view component)
		{
			ProbeSceneIssue issue;
			issue.Code = std::string(ProbeNotRelightableCode);
			issue.Severity = DiagnosticSeverity::Warning;
			issue.Entity = entity.GetUUID();
			issue.Component = std::string(component);
			issue.Field = "Data";
			issue.Message = component == "IrradianceVolume"
				? std::format("the IrradianceVolume of '{}' was baked without the scene's Sky, so the sky does not relight it: it keeps its bake's "
							  "light as the sun turns",
					  entity.GetName())
				: std::format("the ReflectionProbe of '{}' lies in no relightable IrradianceVolume, so the scene's Sky does not dim or brighten it: "
							  "it keeps its bake's brightness as the sun turns",
					  entity.GetName());
			issue.Hint = "Bake the scene's lighting under its Sky with lighting.bake (the editor's Bake Lighting), with an IrradianceVolume around "
						 "each ReflectionProbe, and save the scene.";
			return issue;
		}

		// M45: a relightable volume's box in the world: its world matrix's inverse and its half Size.
		struct RelitBox
		{
			glm::mat4 WorldToLocal = glm::mat4(1.0f);
			glm::vec3 HalfSize = glm::vec3(0.0f);
		};

	}

	std::vector<ProbeSceneIssue> FindProbeSceneIssues(const Scene& scene, AssetManager& assets)
	{
		std::vector<ProbeSceneIssue> issues;
		if (scene.GetRegistry().view<IrradianceVolumeComponent>().empty() && scene.GetRegistry().view<ReflectionProbeComponent>().empty())
			return issues;
		// M45: under a Sky, the relightable volumes' boxes first, so a reflection probe anywhere in the scene finds them.
		const bool sky = Utils::HasActiveSky(scene);
		std::vector<Utils::RelitBox> relitBoxes;
		if (sky)
		{
			scene.ForEachCanonical([&relitBoxes, &assets](ConstEntity entity)
			{
				const IrradianceVolumeComponent* volume = entity.TryGetComponent<IrradianceVolumeComponent>();
				if (volume == nullptr || !entity.IsActive() || !volume->Data.GetHandle().IsValid())
					return;
				const Result<AssetRef<Asset>> loaded = assets.Load(volume->Data.GetHandle());
				const AssetRef<IrradianceVolumeData> data = loaded.has_value() ? AssetCast<IrradianceVolumeData>(*loaded) : AssetRef<IrradianceVolumeData>();
				if (data == nullptr || data->Relight.empty())
					return;
				const glm::mat4 world = TransformSystem::ComputeWorldMatrix(entity);
				if (glm::determinant(glm::mat3(world)) == 0.0f)
					return;
				relitBoxes.push_back({ .WorldToLocal = glm::inverse(world), .HalfSize = volume->Size * 0.5f });
			});
		}
		scene.ForEachCanonical([&issues, &assets, sky, &relitBoxes](ConstEntity entity)
		{
			if (!entity.IsActive())
				return;
			if (const IrradianceVolumeComponent* volume = entity.TryGetComponent<IrradianceVolumeComponent>())
			{
				if (!Utils::LoadAs<IrradianceVolumeData>(assets, volume->Data.GetHandle()).has_value())
					issues.push_back(Utils::MakeProbeNotBaked(entity, "IrradianceVolume", volume->Data.GetHandle()));
				else if (sky)
				{
					const Result<AssetRef<Asset>> loaded = assets.Load(volume->Data.GetHandle());
					if (loaded.has_value() && AssetCast<IrradianceVolumeData>(*loaded)->Relight.empty())
						issues.push_back(Utils::MakeProbeNotRelightable(entity, "IrradianceVolume"));
				}
			}
			if (const ReflectionProbeComponent* probe = entity.TryGetComponent<ReflectionProbeComponent>())
			{
				const AssetHandle data = probe->Data.GetHandle();
				if (const Status loaded = Utils::LoadAs<EnvironmentData>(assets, data); !loaded.has_value())
				{
					issues.push_back(loaded.error().GetCode() == ErrorCode::Unsupported ? Utils::MakeProbeBakeDeferred(entity, data, loaded.error())
																						: Utils::MakeProbeNotBaked(entity, "ReflectionProbe", data));
				}
				else if (sky)
				{
					const glm::vec3 centre(TransformSystem::ComputeWorldMatrix(entity)[3]);
					const bool inside = std::ranges::any_of(relitBoxes, [&centre](const Utils::RelitBox& box)
					{
						const glm::vec3 local(box.WorldToLocal * glm::vec4(centre, 1.0f));
						return glm::all(glm::lessThanEqual(glm::abs(local), box.HalfSize));
					});
					if (!inside)
						issues.push_back(Utils::MakeProbeNotRelightable(entity, "ReflectionProbe"));
				}
			}
		});
		return issues;
	}

}
