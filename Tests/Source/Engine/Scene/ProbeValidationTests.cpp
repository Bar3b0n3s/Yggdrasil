#include "TestsPCH.h"

#include "Engine/Scene/ProbeValidation.h"

#include "Engine/Asset/EnvironmentData.h"
#include "Engine/Asset/IrradianceVolumeData.h"
#include "Engine/Scene/Components/IrradianceVolumeComponent.h"
#include "Engine/Scene/Components/ReflectionProbeComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"
#include "Support/InMemoryAssetManager.h"
#include "Support/SceneTestFixture.h"

#include <string>
#include <utility>
#include <vector>

// The probe checks of a scene (M26, C26.1, R-GI-PROBE; Docs/Decisions/0037-m26-contract.md decision 9, stream Q-A):
// PROBE_NOT_BAKED over a scene's IrradianceVolumes and ReflectionProbes.

namespace Engine {

	namespace {

		constexpr AssetHandle BakedVolume{ 0x7e45000000000001ull };
		constexpr AssetHandle BakedEnvironment{ 0x7e45000000000002ull };
		constexpr AssetHandle MissingData{ 0x7e45000000000003ull };

		// A 2 x 2 x 2 volume of zero probes.
		AssetRef<Asset> MakeBakedVolume()
		{
			IrradianceVolumeData volume;
			volume.Probes.resize(8);
			return CreateRef<IrradianceVolumeData>(std::move(volume));
		}

		IrradianceVolumeComponent MakeVolume(AssetHandle data)
		{
			IrradianceVolumeComponent volume;
			volume.Data = TypedAssetHandle<AssetType::IrradianceVolume>(data);
			return volume;
		}

		ReflectionProbeComponent MakeProbe(AssetHandle data)
		{
			ReflectionProbeComponent probe;
			probe.Data = TypedAssetHandle<AssetType::Environment>(data);
			return probe;
		}

	}

	TEST_SUITE("Scene")
	{
		// Decision 9: a probe whose Data is null, names an asset that is missing, or names an asset of another type (a volume's
		// Data an Environment, a reflection probe's an irradiance volume) lights nothing, so it is PROBE_NOT_BAKED, a warning on
		// its component's Data naming the data that does not load; a probe whose data loads as its type, and a disabled
		// entity's probes, are not reported. Volumes come before reflection probes on one entity, entities in canonical order.
		TEST_CASE("ProbeValidation: a probe whose data is null, missing or of another type is not baked; disabled probes are not checked")
		{
			Test::InMemoryAssetManager assets;
			assets.Publish(BakedVolume, MakeBakedVolume());
			assets.Publish(BakedEnvironment, CreateRef<EnvironmentData>());

			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			const Entity baked = scene.CreateEntity("Baked");
			baked.AddComponent<IrradianceVolumeComponent>(MakeVolume(BakedVolume));
			baked.AddComponent<ReflectionProbeComponent>(MakeProbe(BakedEnvironment));
			const Entity unbaked = scene.CreateEntity("Unbaked");
			unbaked.AddComponent<IrradianceVolumeComponent>();
			unbaked.AddComponent<ReflectionProbeComponent>();
			const Entity missing = scene.CreateEntity("Missing");
			missing.AddComponent<IrradianceVolumeComponent>(MakeVolume(MissingData));
			const Entity swapped = scene.CreateEntity("Swapped");
			swapped.AddComponent<IrradianceVolumeComponent>(MakeVolume(BakedEnvironment));
			swapped.AddComponent<ReflectionProbeComponent>(MakeProbe(BakedVolume));
			const Entity disabled = scene.CreateEntity("Disabled");
			disabled.AddComponent<IrradianceVolumeComponent>();
			disabled.AddComponent<ReflectionProbeComponent>(MakeProbe(MissingData));
			disabled.SetActive(false);

			const std::vector<ProbeSceneIssue> issues = FindProbeSceneIssues(scene, assets);
			REQUIRE(issues.size() == 5);
			for (const ProbeSceneIssue& issue : issues)
			{
				CHECK(issue.Code == ProbeNotBakedCode);
				CHECK(issue.Severity == DiagnosticSeverity::Warning);
				CHECK(issue.Field == "Data");
				CHECK_FALSE(issue.Hint.empty());
			}
			CHECK(issues[0].Entity == unbaked.GetUUID());
			CHECK(issues[0].Component == "IrradianceVolume");
			CHECK(issues[1].Entity == unbaked.GetUUID());
			CHECK(issues[1].Component == "ReflectionProbe");
			CHECK(issues[2].Entity == missing.GetUUID());
			CHECK(issues[2].Component == "IrradianceVolume");
			CHECK(issues[2].Message.find(MissingData.ToString()) != std::string::npos);
			CHECK(issues[3].Entity == swapped.GetUUID());
			CHECK(issues[3].Component == "IrradianceVolume");
			CHECK(issues[3].Message.find(BakedEnvironment.ToString()) != std::string::npos);
			CHECK(issues[4].Entity == swapped.GetUUID());
			CHECK(issues[4].Component == "ReflectionProbe");
			CHECK(issues[4].Message.find(BakedVolume.ToString()) != std::string::npos);

			// A scene without probes has nothing to report.
			Test::SceneTestFixture empty;
			CHECK(FindProbeSceneIssues(empty.GetScene(), assets).empty());
		}
	}

}
