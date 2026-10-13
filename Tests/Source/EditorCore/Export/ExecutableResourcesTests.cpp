#include "TestsPCH.h"

#include "EditorCore/Export/ExecutableResources.h"

#include "Support/EditorTestFixture.h"

namespace Engine {

	TEST_SUITE("EditorCore")
	{
		// ADR 0022 decision 12: the first consumer of Export.Icon, Version and Company (gate 8); M15 patches them into the
		// executable.
		TEST_CASE("ExecutableResources: the export settings' version and company are validated and returned" * doctest::skip(true))
		{
			Test::EditorTestFixture fixture("ExecutableResources");
			fixture.CreateAndOpenProject();
			ExportSettings settings;
			settings.Version = "2.3.4";
			settings.Company = "Fixture Studio";
			const auto resources = ResolveExecutableResources(fixture.GetEditor(), settings);
			REQUIRE_MESSAGE(resources.has_value(), resources.error().ToString());
			CHECK(resources->VersionMajor == 2);
			CHECK(resources->VersionMinor == 3);
			CHECK(resources->VersionPatch == 4);
			CHECK(resources->Company == "Fixture Studio");
			CHECK(resources->Icon.empty());

			for (const char* version : { "2.3", "2.3.4.5", "v2.3.4", "2.03.4", "2.3.65536", "-1.0.0", "" })
			{
				CAPTURE(version);
				settings.Version = version;
				const auto invalid = ResolveExecutableResources(fixture.GetEditor(), settings);
				REQUIRE_FALSE(invalid.has_value());
				CHECK(invalid.error().GetCode() == ErrorCode::Validation);
				CHECK(invalid.error().ToString().find("/Export/Version") != std::string::npos);
			}

			settings.Version = "1.0.0";
			settings.Icon = "Assets/Missing.png";
			const auto missingIcon = ResolveExecutableResources(fixture.GetEditor(), settings);
			REQUIRE_FALSE(missingIcon.has_value());
			CHECK(missingIcon.error().GetCode() == ErrorCode::Validation);
			CHECK(missingIcon.error().ToString().find("/Export/Icon") != std::string::npos);
		}
	}

}
