#include "TestsPCH.h"
#include "Support/GoldenScene.h"

#include "Engine/Graphics/Image.h"
#include "Support/HeadlessGpuFixture.h"

#include <cstddef>
#include <vector>

// Skeletons of the M8 contract (Docs/Decisions/0013-m8-decisions.md decision 14); stream E implements the helpers and
// removes the skips.

namespace Engine {

	TEST_SUITE("Support")
	{
		TEST_CASE("GoldenScene: an image grid places each image row by row" * doctest::skip(true))
		{
			std::vector<Image> images;
			for (uint8_t value = 1; value <= 4; ++value)
			{
				Result<Image> image = CreateImage(2, 1, nvrhi::Format::RGBA8_UNORM);
				REQUIRE(image.has_value());
				std::fill(image->Pixels.begin(), image->Pixels.end(), std::byte{ value });
				images.push_back(std::move(*image));
			}
			const Result<Image> grid = Test::ComposeImageGrid(images, 2);
			REQUIRE_MESSAGE(grid.has_value(), grid.error().ToString());
			CHECK(grid->Width == 4);
			CHECK(grid->Height == 2);
			CHECK(grid->GetRow(0)[0] == std::byte{ 1 });
			CHECK(grid->GetRow(0)[8] == std::byte{ 2 });
			CHECK(grid->GetRow(1)[0] == std::byte{ 3 });
			CHECK(grid->GetRow(1)[8] == std::byte{ 4 });
			CHECK_FALSE(Test::ComposeImageGrid({}, 2).has_value());
			CHECK_FALSE(Test::ComposeImageGrid(images, 0).has_value());
		}

		TEST_CASE("GoldenScene: an unknown golden scene is NotFound" * doctest::test_suite(Test::GpuSuite) * doctest::skip(true))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			const Result<Image> image = Test::RenderGoldenScene(gpu, "NoSuchScene");
			REQUIRE_FALSE(image.has_value());
			CHECK(image.error().GetCode() == ErrorCode::NotFound);
		}
	}

}
