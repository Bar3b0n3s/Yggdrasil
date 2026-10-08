#include "TestsPCH.h"

#include "EditorCore/Automation/ScreenshotMethods.h"

#include "EditorCore/Automation/AutomationServer.h"
#include "EditorCore/EditorContext.h"
#include "Engine/Automation/Protocol/MethodRegistry.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Graphics/Image.h"
#include "Engine/Renderer/RenderSnapshot.h"
#include "Engine/Renderer/ViewportCapture.h"
#include "Support/AutomationTestClient.h"
#include "Support/EditorTestFixture.h"

#include <string>
#include <utility>

// editor.screenshot, and how the editor offers both screenshot methods (without a renderer, with failing captures, in the
// launcher state, in the Runtime subset), in process (Architecture §15.2) with CPU captures injected in place of the
// editor's GPU ones (ScreenshotCaptures). viewport.screenshot's own behaviour is tested with its shared handler in
// Tests/Source/Engine/Automation/Methods/ScreenshotMethodsTests.cpp; the real captures on the GPU (ImGuiScreenshotTests.cpp,
// the golden image "ImGuiDemo") and from Python (Tests/Automation/test_screenshot.py).

namespace Engine {

	namespace {

		// How often the injected captures ran.
		struct CaptureCounts
		{
			uint32_t ViewCalls = 0;
			uint32_t EditorUiCalls = 0;
		};

		// A blank RGBA8 image of the given size.
		Result<Image> MakeBlankImage(uint32_t width, uint32_t height)
		{
			return CreateImage(width, height, nvrhi::Format::RGBA8_UNORM);
		}

		// The editor's server with captures that render blank images (the view at the requested size, the UI at 1600x900,
		// the headless editor's window size) and count their calls in `counts`, which outlives the server.
		AutomationServerSpecification MakeScreenshotServerSpecification(CaptureCounts& counts)
		{
			CaptureCounts* record = &counts;
			AutomationServerSpecification specification = Test::MakeTestServerSpecification();
			specification.RendererName = "vulkan";
			specification.Screenshots.View = [record](const RenderSnapshot& /*snapshot*/, const ViewportScreenshotRequest& request) -> Result<Image>
			{
				++record->ViewCalls;
				return MakeBlankImage(request.Width, request.Height);
			};
			specification.Screenshots.EditorUi = [record]() -> Result<Image>
			{
				++record->EditorUiCalls;
				return MakeBlankImage(1600, 900);
			};
			return specification;
		}

		std::string ReadString(const Json& value)
		{
			return JsonReader(value).ReadString().value_or(std::string());
		}

		uint32_t ReadUnsigned(const Json& value)
		{
			return JsonReader(value).ReadUInt32().value_or(0);
		}

		// A project with the scene Assets/Scenes/Main.scene open, and a client whose server has the CPU captures of `counts`.
		class ScreenshotSetup
		{
		public:
			ScreenshotSetup(std::string_view label, CaptureCounts& counts)
				: m_Fixture(label)
			{
				m_Fixture.CreateAndOpenProject();
				m_Fixture.CreateAndOpenScene();
				m_Client = CreateScope<Test::AutomationTestClient>(m_Fixture.GetEditor(), MakeScreenshotServerSpecification(counts));
			}

			[[nodiscard]] Result<Json> Call(std::string_view method, const Json& params) { return m_Client->Call(method, params); }
		private:
			Test::EditorTestFixture m_Fixture;
			Scope<Test::AutomationTestClient> m_Client;
		};

	}

	TEST_SUITE("EditorCore")
	{
		TEST_CASE("ScreenshotMethods: editor.screenshot renders the editor UI capture, downscaled to maxDimension")
		{
			CaptureCounts counts;
			ScreenshotSetup setup("EditorScreenshot", counts);

			const Result<Json> shot = setup.Call("editor.screenshot", Json::object());
			REQUIRE_MESSAGE(shot.has_value(), shot.error().ToString());
			CHECK(counts.EditorUiCalls == 1);
			CHECK((*shot)["mimeType"] == Json("image/png"));
			CHECK(ReadUnsigned((*shot)["width"]) == 1024);
			CHECK(ReadUnsigned((*shot)["height"]) == 576);
			const Result<Image> downscaled = ReadPng(FileSystem::PathFromUtf8(ReadString((*shot)["path"])));
			REQUIRE(downscaled.has_value());
			CHECK(downscaled->Width == 1024);

			const Result<Json> full = setup.Call("editor.screenshot", Json{ { "maxDimension", 2000 } });
			REQUIRE_MESSAGE(full.has_value(), full.error().ToString());
			CHECK(ReadUnsigned((*full)["width"]) == 1600);
			CHECK(ReadUnsigned((*full)["height"]) == 900);
		}

		TEST_CASE("ScreenshotMethods: without captures (--renderer none) both methods are Unsupported")
		{
			Test::AutomationFixture setup("ScreenshotNoRenderer");
			for (const auto& [method, params] : { std::pair{ "viewport.screenshot", Json{ { "view", "scene" } } }, std::pair{ "editor.screenshot", Json::object() } })
			{
				INFO(method);
				const Result<Json> shot = setup.Call(method, params);
				REQUIRE_FALSE(shot.has_value());
				CHECK(shot.error().GetCode() == ErrorCode::Unsupported);
				CHECK(shot.error().ToString().contains("--renderer none"));
			}
		}

		TEST_CASE("ScreenshotMethods: a failed capture is the method's error, with what was rendering as context")
		{
			Test::EditorTestFixture fixture("ScreenshotFailure");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			AutomationServerSpecification specification = Test::MakeTestServerSpecification();
			specification.Screenshots.View = [](const RenderSnapshot& /*snapshot*/, const ViewportScreenshotRequest& /*request*/) -> Result<Image>
			{
				return MakeError(ErrorCode::Gpu, "the device ran out of memory");
			};
			specification.Screenshots.EditorUi = []() -> Result<Image>
			{
				return MakeError(ErrorCode::InvalidState, "no UI frame was rendered yet");
			};
			Test::AutomationTestClient client(fixture.GetEditor(), specification);

			const Json viewport = client.Request("viewport.screenshot", Json{ { "view", "scene" } });
			REQUIRE(viewport.contains("error"));
			// A Gpu error is Internal on the wire (ADR 0008 decision 5).
			CHECK(viewport["error"]["data"]["errorCode"] == Json("Gpu"));
			CHECK(viewport["error"]["data"].dump().contains("while rendering the viewport"));

			const Result<Json> editor = client.Call("editor.screenshot", Json::object());
			REQUIRE_FALSE(editor.has_value());
			CHECK(editor.error().GetCode() == ErrorCode::InvalidState);
		}

		TEST_CASE("ScreenshotMethods: the screenshots are not available in the launcher state")
		{
			Test::EditorTestFixture fixture("ScreenshotLauncher");
			CaptureCounts counts;
			Test::AutomationTestClient client(fixture.GetEditor(), MakeScreenshotServerSpecification(counts));
			for (const auto& [method, params] : { std::pair{ "viewport.screenshot", Json{ { "view", "scene" } } }, std::pair{ "editor.screenshot", Json::object() } })
			{
				INFO(method);
				const Result<Json> shot = client.Call(method, params);
				REQUIRE_FALSE(shot.has_value());
				CHECK(shot.error().GetCode() == ErrorCode::InvalidState);
			}
			CHECK(counts.ViewCalls == 0);
			CHECK(counts.EditorUiCalls == 0);
		}

		TEST_CASE("ScreenshotMethods: viewport.screenshot is in the Runtime subset, editor.screenshot is not")
		{
			Test::AutomationFixture setup("ScreenshotRuntimeSubset");
			const MethodDescriptor* viewport = setup.GetClient().GetServer().GetMethods().Find("viewport.screenshot");
			const MethodDescriptor* editor = setup.GetClient().GetServer().GetMethods().Find("editor.screenshot");
			REQUIRE(viewport != nullptr);
			REQUIRE(editor != nullptr);
			CHECK(viewport->Specification.AvailableInRuntime);
			CHECK(viewport->Specification.ExposeAsTool);
			CHECK_FALSE(viewport->Specification.Mutates);
			CHECK_FALSE(editor->Specification.AvailableInRuntime);
		}
	}

}
