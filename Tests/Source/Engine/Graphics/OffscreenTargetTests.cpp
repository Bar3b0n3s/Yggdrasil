#include "TestsPCH.h"

#include "Engine/Graphics/OffscreenTarget.h"

#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/Readback.h"
#include "Support/HeadlessGpuFixture.h"

namespace Engine {

	TEST_SUITE("Graphics")
	{
		TEST_CASE("OffscreenTarget: creates its attachments, resizes and clears" * doctest::test_suite(Test::GpuSuite) * doctest::skip(true))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			Result<OffscreenTarget> created = OffscreenTarget::Create(device, { .Width = 32, .Height = 16, .Depth = true, .DebugName = "Test" });
			REQUIRE_MESSAGE(created.has_value(), created.error().ToString());
			OffscreenTarget target = std::move(*created);
			CHECK(target.GetWidth() == 32);
			CHECK(target.GetHeight() == 16);
			REQUIRE(target.GetColorTexture() != nullptr);
			REQUIRE(target.GetDepthTexture() != nullptr);
			REQUIRE(target.GetFramebuffer() != nullptr);
			CHECK(target.GetColorTexture()->getDesc().format == nvrhi::Format::RGBA8_UNORM);
			CHECK(target.GetDepthTexture()->getDesc().format == nvrhi::Format::D32);
			CHECK(std::string(target.GetColorTexture()->getDesc().debugName) == "Test.Color");

			REQUIRE(target.Resize(device, 48, 24).has_value());
			CHECK(target.GetWidth() == 48);
			CHECK(target.GetColorTexture()->getDesc().width == 48);
			CHECK(target.GetFramebuffer()->getFramebufferInfo().width == 48);

			// Zero sizes and a format that cannot be rendered to are rejected.
			const Result<OffscreenTarget> empty = OffscreenTarget::Create(device, { .Width = 0, .Height = 16 });
			REQUIRE_FALSE(empty.has_value());
			CHECK(empty.error().GetCode() == ErrorCode::InvalidArgument);
			const Result<OffscreenTarget> compressed = OffscreenTarget::Create(device, { .Width = 16, .Height = 16, .ColorFormat = nvrhi::Format::BC1_UNORM });
			REQUIRE_FALSE(compressed.has_value());
			CHECK(compressed.error().GetCode() == ErrorCode::InvalidArgument);
		}
	}

}
