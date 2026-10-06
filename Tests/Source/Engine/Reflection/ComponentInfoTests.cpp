#include "TestsPCH.h"

#include "Engine/Reflection/ComponentInfo.h"

#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Reflection/TypeRegistry.h"

#include <nlohmann/json.hpp>

namespace Engine {

	namespace {

		struct VersionedComponent
		{
			float Radius = 1.0f;
		};

		struct OtherComponent
		{
			bool Enabled = true;
		};

	}

	// Version 1 stored "Size"; version 2 renamed it "Diameter"; version 3 stores "Radius" (half of it).
	static Status MigrateVersionedFrom1(Json& component)
	{
		if (!component.is_object() || !component.contains("Size"))
			return MakeError(ErrorCode::Validation, "expected 'Size'");
		component["Diameter"] = component["Size"];
		component.erase("Size");
		return {};
	}

	static Status MigrateVersionedFrom2(Json& component)
	{
		if (!component.is_object() || !component.contains("Diameter") || !component["Diameter"].is_number())
			return MakeError(ErrorCode::Validation, "expected a numeric 'Diameter'");
		const JsonReader reader(component["Diameter"], "/Diameter");
		const Result<float> diameter = reader.ReadFloat();
		if (!diameter)
			return std::unexpected(diameter.error());
		component["Radius"] = *diameter * 0.5f;
		component.erase("Diameter");
		return {};
	}

	static Scope<TypeRegistry> CreateComponentTestRegistry()
	{
		Scope<TypeRegistry> registry = CreateScope<TypeRegistry>();
		registry->Component<VersionedComponent>("Versioned", "A component with two migrations.")
			.Category("Test")
			.Version(3)
			.Flags(ComponentFlags::UniquePerScene)
			.RemoveFlags(ComponentFlags::Removable)
			.Requires<OtherComponent>()
			.Migration(1, &MigrateVersionedFrom1)
			.Migration(2, &MigrateVersionedFrom2)
			.Field("Radius", &VersionedComponent::Radius, "The radius.", { .Min = 0.001, .Unit = "m" });
		registry->Component<OtherComponent>("Other", "A component other components require.")
			.Category("Test")
			.Field("Enabled", &OtherComponent::Enabled, "Whether it is enabled.");
		registry->Freeze();
		return registry;
	}

	TEST_SUITE("Reflection")
	{
		TEST_CASE("ComponentInfo: flags, category and version are what the builder set" * doctest::skip(true))
		{
			const Scope<TypeRegistry> registry = CreateComponentTestRegistry();
			const ComponentInfo* versioned = registry->FindComponent("Versioned");
			REQUIRE(versioned != nullptr);
			CHECK(versioned->GetVersion() == 3);
			CHECK(versioned->GetCategory() == "Test");
			CHECK(versioned->HasFlag(ComponentFlags::UniquePerScene));
			CHECK(versioned->HasFlag(ComponentFlags::Serializable));
			CHECK_FALSE(versioned->HasFlag(ComponentFlags::Removable));
			CHECK(versioned->GetHostOps() == nullptr);

			REQUIRE(versioned->GetRequires().size() == 1);
			CHECK(versioned->GetRequires()[0] == registry->FindComponent("Other")); // resolved by Freeze although registered later
			CHECK(versioned->GetExcludes().empty());

			const ComponentInfo* other = registry->FindComponent("Other");
			REQUIRE(other != nullptr);
			CHECK(other->GetFlags() == ComponentFlags::Default);
			CHECK(other->GetVersion() == 1);
			CHECK(other->GetIndex() == 1);
		}

		TEST_CASE("ComponentInfo: Migrate runs the chain from any older version" * doctest::skip(true))
		{
			const Scope<TypeRegistry> registry = CreateComponentTestRegistry();
			const ComponentInfo* versioned = registry->FindComponent("Versioned");
			REQUIRE(versioned != nullptr);

			Result<Json> fromVersion1 = JsonReader::Parse(R"({ "Size": 4 })");
			REQUIRE(fromVersion1.has_value());
			REQUIRE(versioned->Migrate(1, *fromVersion1).has_value());
			CHECK(*fromVersion1 == *JsonReader::Parse(R"({ "Radius": 2 })"));

			Result<Json> current = JsonReader::Parse(R"({ "Radius": 3 })");
			REQUIRE(current.has_value());
			REQUIRE(versioned->Migrate(3, *current).has_value());
			CHECK(*current == *JsonReader::Parse(R"({ "Radius": 3 })"));
		}

		TEST_CASE("ComponentInfo: Migrate rejects newer and invalid versions without changing the input" * doctest::skip(true))
		{
			const Scope<TypeRegistry> registry = CreateComponentTestRegistry();
			const ComponentInfo* versioned = registry->FindComponent("Versioned");
			REQUIRE(versioned != nullptr);

			Result<Json> json = JsonReader::Parse(R"({ "Radius": 3 })");
			REQUIRE(json.has_value());
			const Json original = *json;

			const Status newer = versioned->Migrate(4, *json);
			REQUIRE_FALSE(newer.has_value());
			CHECK(newer.error().GetCode() == ErrorCode::UnsupportedVersion);
			CHECK(newer.error().GetMessageText().find('4') != std::string::npos);
			CHECK(newer.error().GetMessageText().find('3') != std::string::npos);

			const Status zero = versioned->Migrate(0, *json);
			REQUIRE_FALSE(zero.has_value());
			CHECK(zero.error().GetCode() == ErrorCode::Validation);

			const Status malformed = versioned->Migrate(1, *json); // version 1 data must have "Size"
			CHECK_FALSE(malformed.has_value());
			CHECK(*json == original);
		}
	}

}
