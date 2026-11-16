#include "TestsPCH.h"

#include "Engine/Asset/AssetType.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Reflection/ValidationContext.h"
#include "Engine/Scene/Components/BuiltinComponents.h"
#include "Support/SceneTestFixture.h"

#include <cstddef>
#include <string_view>

// The Probes category (M26, C26.1, R-GI-PROBE; Docs/Decisions/0037-m26-contract.md decisions 3 and 4): the contract registers
// the IrradianceVolume and the ReflectionProbe, so these cases are live; their behaviour is frozen by the skipped cases of
// the bake and the shading.

namespace Engine {

	namespace {

		const ComponentInfo& RequireProbesComponent(const TypeRegistry& registry, std::string_view name)
		{
			const ComponentInfo* info = registry.FindComponent(name);
			REQUIRE_MESSAGE(info != nullptr, name);
			return *info;
		}

	}

	TEST_SUITE("Scene")
	{
		TEST_CASE("ProbesRegistration: the probes are registered after the ParticleEmitter, a reflection probe's data an Environment")
		{
			const Scope<TypeRegistry> registry = Test::CreateBuiltinRegistry();
			const ComponentInfo& emitter = RequireProbesComponent(*registry, "ParticleEmitter");
			const ComponentInfo& volume = RequireProbesComponent(*registry, "IrradianceVolume");
			const ComponentInfo& probe = RequireProbesComponent(*registry, "ReflectionProbe");
			// After the Particles category, which ends with M33's ParticleLook (Docs/Decisions/0048-m33-particle-look.md decision 1).
			CHECK(volume.GetIndex() == RequireProbesComponent(*registry, "ParticleLook").GetIndex() + 1);
			CHECK(volume.GetIndex() > emitter.GetIndex());
			CHECK(probe.GetIndex() == volume.GetIndex() + 1);
			for (const ComponentInfo* info : { &volume, &probe })
			{
				CAPTURE(info->GetName());
				CHECK(info->GetCategory() == "Probes");
				CHECK(info->GetVersion() == 1);
				REQUIRE(info->GetRequires().size() == 1);
				CHECK(info->GetRequires()[0] == registry->FindComponent("Transform"));
				CHECK(info->GetExcludes().empty());
			}
			// Decision 3: the IrradianceVolume asset type, and no ReflectionProbe type (its bake is an Environment).
			CHECK(volume.FindField("Data")->GetMeta().AssetFilter == "IrradianceVolume");
			CHECK(probe.FindField("Data")->GetMeta().AssetFilter == "Environment");
			CHECK_FALSE(AssetTypeFromString("ReflectionProbe").has_value());
		}

		TEST_CASE("ProbesRegistration: a reflection probe's capture resolution is a power of two")
		{
			const Scope<TypeRegistry> registry = Test::CreateBuiltinRegistry();
			const ComponentInfo& info = RequireProbesComponent(*registry, "ReflectionProbe");
			ResolveContext resolve;
			resolve.Registry = registry.get();
			const auto countErrors = [&resolve, &info](const ReflectionProbeComponent& probe)
			{
				ValidationContext context;
				info.Validate(&probe, resolve, context);
				return context.GetErrorCount();
			};
			ReflectionProbeComponent probe;
			CHECK(countErrors(probe) == 0);
			probe.Resolution = 96;
			CHECK(countErrors(probe) == 1);
			probe.Resolution = MaxReflectionProbeResolution;
			CHECK(countErrors(probe) == 0);
		}
	}

}
