#include "TestsPCH.h"

#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Reflection/ValidationContext.h"
#include "Engine/Scene/Components/BuiltinComponents.h"
#include "Support/SceneTestFixture.h"

#include <cstddef>
#include <string_view>

// The Particles category (M26, C26.1, C-PART; Docs/Decisions/0037-m26-contract.md decision 4): the contract registers the
// ParticleEmitter, so these cases are live; its behaviour is frozen by the skipped cases of the simulation and the pass.

namespace Engine {

	namespace {

		const ComponentInfo& RequireParticlesComponent(const TypeRegistry& registry, std::string_view name)
		{
			const ComponentInfo* info = registry.FindComponent(name);
			REQUIRE_MESSAGE(info != nullptr, name);
			return *info;
		}

		size_t CountParticlesErrors(const TypeRegistry& registry, const ComponentInfo& info, const void* component)
		{
			ResolveContext resolve;
			resolve.Registry = &registry;
			ValidationContext context;
			info.Validate(component, resolve, context);
			return context.GetErrorCount();
		}

	}

	TEST_SUITE("Scene")
	{
		TEST_CASE("ParticlesRegistration: the ParticleEmitter is registered after Animation, requiring Transform")
		{
			const Scope<TypeRegistry> registry = Test::CreateBuiltinRegistry();
			const ComponentInfo& skinned = RequireParticlesComponent(*registry, "SkinnedMesh");
			const ComponentInfo& emitter = RequireParticlesComponent(*registry, "ParticleEmitter");
			// ADR 0028's phase-3 ledger: M26's categories after Animation, Particles first; M24 adds none, so the relative order
			// only.
			CHECK(emitter.GetIndex() > skinned.GetIndex());
			CHECK(emitter.GetCategory() == "Particles");
			CHECK(emitter.GetVersion() == 1);
			REQUIRE(emitter.GetRequires().size() == 1);
			CHECK(emitter.GetRequires()[0] == registry->FindComponent("Transform"));
			CHECK(emitter.GetExcludes().empty());
			CHECK(emitter.FindField("Texture")->GetMeta().AssetFilter == "Texture");
			REQUIRE(emitter.FindField("ColorOverLife") != nullptr);
			CHECK(emitter.FindField("ColorOverLife")->GetKind() == FieldType::Array);
			CHECK(emitter.FindField("Shape")->GetKind() == FieldType::Enum);
			CHECK(emitter.FindField("Lifetime")->GetKind() == FieldType::Vec2);
			REQUIRE(registry->FindStruct("ParticleColorKey") != nullptr);
			REQUIRE(registry->FindStruct("ParticleSizeKey") != nullptr);
		}

		TEST_CASE("ParticlesRegistration: an emitter's ranges are ordered, its burst within its cap and its curves short and ascending")
		{
			const Scope<TypeRegistry> registry = Test::CreateBuiltinRegistry();
			const ComponentInfo& info = RequireParticlesComponent(*registry, "ParticleEmitter");
			ParticleEmitterComponent emitter;
			CHECK(CountParticlesErrors(*registry, info, &emitter) == 0);
			emitter.Lifetime = glm::vec2(3.0f, 2.0f);
			CHECK(CountParticlesErrors(*registry, info, &emitter) == 1);
			emitter.Lifetime = glm::vec2(1.0f, 2.0f);
			emitter.Burst = emitter.MaxParticles + 1;
			CHECK(CountParticlesErrors(*registry, info, &emitter) == 1);
			emitter.Burst = 0;
			emitter.ColorOverLife = { { .Time = 0.5f }, { .Time = 0.25f } };
			CHECK(CountParticlesErrors(*registry, info, &emitter) == 1);
			emitter.ColorOverLife.assign(MaxParticleCurveKeys + 1, ParticleColorKey{});
			CHECK(CountParticlesErrors(*registry, info, &emitter) == 1);
			emitter.ColorOverLife.clear();
			emitter.SizeOverLife = { { .Time = 0.0f, .Size = 1.0f }, { .Time = 1.0f, .Size = 0.0f } };
			CHECK(CountParticlesErrors(*registry, info, &emitter) == 0);
		}
	}

}
