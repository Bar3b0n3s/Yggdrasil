#include "TestsPCH.h"
#include "EditorCore/Automation/ScreenshotMethods.h"

#include "EditorCore/Automation/Private/M10MethodFixture.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Graphics/Image.h"

namespace Engine {

	TEST_SUITE("Automation")
	{
		TEST_CASE("FreshEditorScreenshot: admission waits for a strictly newer completed frame before encoding")
		{
			uint64_t frame = 7;
			uint32_t requested = 0;
			uint32_t captured = 0;
			auto specification = Test::MakeTestServerSpecification();
			specification.Screenshots.CompletedUiFrame = [&frame]()
			{
				return frame;
			};
			specification.Screenshots.RequestUiFrame = [&requested]() -> Status
			{
				++requested;
				return {};
			};
			specification.Screenshots.EditorUi = [&captured]() -> Result<Image>
			{
				++captured;
				return CreateImage(80, 40, nvrhi::Format::RGBA8_UNORM);
			};
			Test::M10MethodFixture setup(specification);
			auto context = setup.Context("editor.screenshot", Json{ { "maxDimension", 40 } });
			MethodResult started = setup.Methods.Invoke(*context);
			REQUIRE(std::holds_alternative<Scope<PendingOperation>>(started));
			auto& operation = std::get<Scope<PendingOperation>>(started);
			CHECK(requested == 1);
			CHECK_FALSE(operation->Poll(*context).has_value());
			CHECK_FALSE(operation->Poll(*context).has_value());
			CHECK(captured == 0);
			++frame;
			const auto result = operation->Poll(*context);
			REQUIRE(result.has_value());
			REQUIRE(result->has_value());
			CHECK(captured == 1);
			CHECK((**result)["width"] == 40);
			CHECK((**result)["height"] == 20);
			const auto path = JsonReader((**result)["path"]).ReadString();
			REQUIRE(path.has_value());
			const auto image = ReadPng(FileSystem::PathFromUtf8(*path));
			REQUIRE(image.has_value());
			CHECK(image->Width == 40);
		}

		TEST_CASE("FreshEditorScreenshot: cancellation and frame sequence reset never capture stale pixels")
		{
			uint64_t frame = 5;
			uint32_t captured = 0;
			auto specification = Test::MakeTestServerSpecification();
			specification.Screenshots.CompletedUiFrame = [&frame]()
			{
				return frame;
			};
			specification.Screenshots.RequestUiFrame = []() -> Status
			{
				return {};
			};
			specification.Screenshots.EditorUi = [&captured]() -> Result<Image>
			{
				++captured;
				return CreateImage(1, 1, nvrhi::Format::RGBA8_UNORM);
			};
			Test::M10MethodFixture setup(specification);
			auto context = setup.Context("editor.screenshot");
			auto operation = Automation::BeginEditorScreenshot(*context, {});
			REQUIRE(operation.has_value());
			SUBCASE("cancel")
			{
				(*operation)->Cancel(*context);
			}
			SUBCASE("frame reset")
			{
				frame = 0;
			}
			const auto result = (*operation)->Poll(*context);
			REQUIRE(result.has_value());
			REQUIRE_FALSE(result->has_value());
			CHECK(result->error().GetCode() == ErrorCode::Cancelled);
			CHECK(captured == 0);
		}

		TEST_CASE("FreshEditorScreenshot: a frame whose viewport images were withdrawn after it was built is never captured")
		{
			uint64_t frame = 3;
			uint32_t requested = 0;
			uint32_t captured = 0;
			bool current = true;
			auto specification = Test::MakeTestServerSpecification();
			specification.Screenshots.CompletedUiFrame = [&frame]()
			{
				return frame;
			};
			specification.Screenshots.RequestUiFrame = [&requested]() -> Status
			{
				++requested;
				return {};
			};
			specification.Screenshots.IsUiFrameCurrent = [&current]()
			{
				return current;
			};
			specification.Screenshots.EditorUi = [&captured, &current]() -> Result<Image>
			{
				CHECK(current); // the captured draw data still references every viewport texture it draws
				++captured;
				return CreateImage(8, 8, nvrhi::Format::RGBA8_UNORM);
			};
			Test::M10MethodFixture setup(specification);
			auto context = setup.Context("editor.screenshot");
			auto operation = Automation::BeginEditorScreenshot(*context, {});
			REQUIRE(operation.has_value());
			CHECK(requested == 1);
			// A newer frame completed, then the window was minimized and its viewport textures were withdrawn.
			++frame;
			current = false;
			CHECK_FALSE((*operation)->Poll(*context).has_value());
			CHECK(captured == 0);
			CHECK(requested == 2); // another frame was requested
			// That stale frame can never satisfy the request, even once the host's state is current again.
			current = true;
			CHECK_FALSE((*operation)->Poll(*context).has_value());
			CHECK(captured == 0);
			++frame;
			const auto result = (*operation)->Poll(*context);
			REQUIRE(result.has_value());
			REQUIRE(result->has_value());
			CHECK(captured == 1);
		}

		TEST_CASE("FreshEditorScreenshot: a frame request the host cannot schedule fails the request at once")
		{
			uint64_t frame = 2;
			bool fail = true;
			uint32_t captured = 0;
			auto specification = Test::MakeTestServerSpecification();
			specification.Screenshots.CompletedUiFrame = [&frame]()
			{
				return frame;
			};
			specification.Screenshots.RequestUiFrame = [&fail]() -> Status
			{
				if (fail)
					return MakeError(ErrorCode::Unsupported, "the host has no ImGui layer to render an offscreen frame");
				return {};
			};
			specification.Screenshots.IsUiFrameCurrent = [&fail]()
			{
				return !fail;
			};
			specification.Screenshots.EditorUi = [&captured]() -> Result<Image>
			{
				++captured;
				return CreateImage(8, 8, nvrhi::Format::RGBA8_UNORM);
			};
			SUBCASE("at admission")
			{
				Test::M10MethodFixture setup(specification);
				auto context = setup.Context("editor.screenshot");
				const auto operation = Automation::BeginEditorScreenshot(*context, {});
				REQUIRE_FALSE(operation.has_value());
				CHECK(operation.error().GetCode() == ErrorCode::Unsupported);
				// Through the registered method the client gets the error now, not a Timeout at the 60 s deadline.
				const auto shot = setup.Call("editor.screenshot");
				REQUIRE_FALSE(shot.has_value());
				CHECK(shot.error().GetCode() == ErrorCode::Unsupported);
			}
			SUBCASE("while requesting a replacement frame")
			{
				fail = false;
				Test::M10MethodFixture setup(specification);
				auto context = setup.Context("editor.screenshot");
				auto operation = Automation::BeginEditorScreenshot(*context, {});
				REQUIRE(operation.has_value());
				fail = true;
				++frame;
				const auto result = (*operation)->Poll(*context);
				REQUIRE(result.has_value());
				REQUIRE_FALSE(result->has_value());
				CHECK(result->error().GetCode() == ErrorCode::Unsupported);
			}
			CHECK(captured == 0);
		}

		TEST_CASE("FreshEditorScreenshot: missing providers and capture failures retain their errors")
		{
			uint64_t frame = 0;
			auto specification = Test::MakeTestServerSpecification();
			specification.Screenshots.CompletedUiFrame = [&frame]()
			{
				return frame;
			};
			specification.Screenshots.RequestUiFrame = [&frame]() -> Status
			{
				++frame;
				return {};
			};
			specification.Screenshots.EditorUi = []() -> Result<Image>
			{
				return MakeError(ErrorCode::Io, "capture failed");
			};
			SUBCASE("capture failure")
			{
				Test::M10MethodFixture setup(specification);
				auto context = setup.Context("editor.screenshot");
				auto operation = Automation::BeginEditorScreenshot(*context, {});
				REQUIRE(operation.has_value());
				const auto result = (*operation)->Poll(*context);
				REQUIRE(result.has_value());
				CHECK(result->error().GetCode() == ErrorCode::Io);
			}
			SUBCASE("missing freshness provider")
			{
				specification.Screenshots.CompletedUiFrame = {};
				Test::M10MethodFixture setup(specification);
				auto context = setup.Context("editor.screenshot");
				CHECK(Automation::BeginEditorScreenshot(*context, {}).error().GetCode() == ErrorCode::Unsupported);
				CHECK(frame == 0);
			}
		}
	}

}
