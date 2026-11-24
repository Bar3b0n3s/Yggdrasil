#include "TestsPCH.h"

#include "Engine/Scene/ProbeValidation.h"

#include "Engine/Asset/EnvironmentData.h"
#include "Engine/Asset/IrradianceVolumeData.h"
#include "Engine/Scene/Components/IrradianceVolumeComponent.h"
#include "Engine/Scene/Components/ReflectionProbeComponent.h"
#include "Engine/Scene/Components/SkyComponent.h"
#include "Engine/Scene/Components/TransformComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/SkyValidation.h"
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
		constexpr AssetHandle RelitVolume{ 0x7e45000000000004ull };

		// A 2 x 2 x 2 volume of zero probes.
		AssetRef<Asset> MakeBakedVolume()
		{
			IrradianceVolumeData volume;
			volume.Probes.resize(8);
			return CreateRef<IrradianceVolumeData>(std::move(volume));
		}

		// MakeBakedVolume with a relight block (M45's format 2).
		AssetRef<Asset> MakeRelitVolume()
		{
			IrradianceVolumeData volume;
			volume.Probes.resize(8);
			volume.Relight.resize(8);
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

		// M45 (Docs/Decisions/0063-m45-sky-and-time-of-day.md decision 3): under an enabled Sky, a volume whose data loads without a
		// relight block, and a reflection probe whose data loads and whose centre lies in no relightable volume's box (an
		// unrelightable volume's box does not count), are PROBE_NOT_RELIGHTABLE on their Data, in canonical order, with the bake as
		// the hint; a probe that is not baked is PROBE_NOT_BAKED alone; without an enabled Sky nothing is relightable to report.
		TEST_CASE("ProbeValidation: a volume without a relight block under a Sky is reported")
		{
			Test::InMemoryAssetManager assets;
			assets.Publish(BakedVolume, MakeBakedVolume());
			assets.Publish(RelitVolume, MakeRelitVolume());
			assets.Publish(BakedEnvironment, CreateRef<EnvironmentData>());

			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			const auto place = [](Entity entity, const glm::vec3& position)
			{
				entity.GetComponent<TransformComponent>().Translation = position;
				return entity;
			};
			const Entity plain = place(scene.CreateEntity("Plain"), glm::vec3(0.0f));
			plain.AddComponent<IrradianceVolumeComponent>(MakeVolume(BakedVolume));
			const Entity relit = place(scene.CreateEntity("Relit"), glm::vec3(100.0f, 0.0f, 0.0f));
			relit.AddComponent<IrradianceVolumeComponent>(MakeVolume(RelitVolume));
			// Inside the relit volume's 10 x 4 x 10 box; inside only the plain volume's; outside both.
			place(scene.CreateEntity("Inside"), glm::vec3(103.0f, 1.0f, -4.0f)).AddComponent<ReflectionProbeComponent>(MakeProbe(BakedEnvironment));
			const Entity plainOnly = place(scene.CreateEntity("PlainOnly"), glm::vec3(1.0f, 0.0f, 1.0f));
			plainOnly.AddComponent<ReflectionProbeComponent>(MakeProbe(BakedEnvironment));
			const Entity outside = place(scene.CreateEntity("Outside"), glm::vec3(106.0f, 0.0f, 0.0f));
			outside.AddComponent<ReflectionProbeComponent>(MakeProbe(BakedEnvironment));
			const Entity unbaked = scene.CreateEntity("Unbaked");
			unbaked.AddComponent<IrradianceVolumeComponent>();
			const Entity sky = scene.CreateEntity("Sky");
			sky.AddComponent<SkyComponent>();

			std::vector<ProbeSceneIssue> issues = FindProbeSceneIssues(scene, assets);
			REQUIRE(issues.size() == 4);
			CHECK(issues[0].Code == ProbeNotRelightableCode);
			CHECK(issues[0].Entity == plain.GetUUID());
			CHECK(issues[0].Component == "IrradianceVolume");
			CHECK(issues[1].Code == ProbeNotRelightableCode);
			CHECK(issues[1].Entity == plainOnly.GetUUID());
			CHECK(issues[1].Component == "ReflectionProbe");
			CHECK(issues[2].Code == ProbeNotRelightableCode);
			CHECK(issues[2].Entity == outside.GetUUID());
			CHECK(issues[3].Code == ProbeNotBakedCode);
			CHECK(issues[3].Entity == unbaked.GetUUID());
			for (const ProbeSceneIssue& issue : issues)
			{
				CHECK(issue.Severity == DiagnosticSeverity::Warning);
				CHECK(issue.Field == "Data");
				CHECK(issue.Hint.find("lighting.bake") != std::string::npos);
			}

			// Without an enabled Sky, only the bake that is missing.
			sky.SetActive(false);
			issues = FindProbeSceneIssues(scene, assets);
			REQUIRE(issues.size() == 1);
			CHECK(issues[0].Code == ProbeNotBakedCode);
		}
	}

}
