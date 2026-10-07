#include "TestsPCH.h"
#include "Support/AssetTestFixture.h"

#include "Engine/Graphics/Image.h"

namespace Engine {

	TEST_SUITE("Support")
	{
		TEST_CASE("AssetTestFixture: memory mounts hold the written files")
		{
			Test::AssetTestFixture fixture;
			fixture.WriteProjectText("Assets/Notes/Readme.txt", "hello");
			const Buffer bytes = fixture.ReadProjectFile("Assets/Notes/Readme.txt");
			CHECK(AsStringView(bytes) == "hello");
			CHECK(fixture.GetVfs().IsMounted("cache"));
			CHECK(fixture.GetRegistry().IsFrozen());
			CHECK(fixture.GetJobSystem().IsInline());
		}

		TEST_CASE("AssetTestFixture: the job system has the workers the test asks for")
		{
			Test::AssetTestFixture threaded(1, 2);
			CHECK_FALSE(threaded.GetJobSystem().IsInline());
			CHECK(threaded.GetJobSystem().GetWorkerCount() == 2);
		}

		TEST_CASE("AssetTestFixture: test PNGs decode to their size")
		{
			const Buffer png = Test::MakeTestPng(8, 4, 3);
			Result<Image> decoded = DecodePng(png);
			REQUIRE_MESSAGE(decoded.has_value(), decoded.error().ToString());
			CHECK(decoded->Width == 8);
			CHECK(decoded->Height == 4);
			CHECK(Test::MakeTestPng(8, 4, 3) == png);
		}

		TEST_CASE("AssetTestFixture: opening the project scans an empty Assets folder")
		{
			Test::AssetTestFixture fixture;
			const AssetRefreshReport report = fixture.OpenProject(false);
			CHECK(report.MetaCount == 0);
			CHECK(report.CreatedMetas.empty());
			CHECK(fixture.GetManager().HasProject());
		}
	}

}
