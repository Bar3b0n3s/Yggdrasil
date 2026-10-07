#include "TestsPCH.h"

#include "Engine/Graphics/OffscreenTarget.h"

#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/Readback.h"
#include "Support/HeadlessGpuFixture.h"

namespace Engine {

	TEST_SUITE("Graphics")
	{
		TEST_CASE("OffscreenTarget: creates its attachments, resizes and clears" * doctest::test_suite(Test::GpuSuite))
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
			const Result<OffscreenTarget> depthAsColor = OffscreenTarget::Create(device, { .Width = 16, .Height = 16, .ColorFormat = nvrhi::Format::D32 });
			REQUIRE_FALSE(depthAsColor.has_value());
			CHECK(depthAsColor.error().GetCode() == ErrorCode::InvalidArgument);
		}

		TEST_CASE("OffscreenTarget: resizing to the current size keeps the textures, and a failed resize keeps the target"
			* doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			Result<OffscreenTarget> created = OffscreenTarget::Create(device, { .Width = 32, .Height = 16, .Depth = true });
			REQUIRE_MESSAGE(created.has_value(), created.error().ToString());
			OffscreenTarget target = std::move(*created);
			const nvrhi::ITexture* color = target.GetColorTexture();
			const nvrhi::IFramebuffer* framebuffer = target.GetFramebuffer();

			REQUIRE(target.Resize(device, 32, 16).has_value());
			CHECK(target.GetColorTexture() == color);
			CHECK(target.GetFramebuffer() == framebuffer);

			const Status failed = target.Resize(device, 0, 16);
			REQUIRE_FALSE(failed.has_value());
			CHECK(failed.error().GetCode() == ErrorCode::InvalidArgument);
			CHECK(target.GetWidth() == 32);
			CHECK(target.GetColorTexture() == color);
			CHECK(target.GetDepthTexture() != nullptr);
		}
	}

}
