#include "TestsPCH.h"

#include "EditorCore/Automation/ScreenshotMethods.h"

#include "EditorCore/Automation/AutomationServer.h"
#include "EditorCore/Automation/EditorMethodContext.h"
#include "EditorCore/EditorContext.h"
#include "Engine/Automation/Protocol/MethodRegistry.h"
#include "Engine/Automation/Protocol/PendingOperation.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Graphics/Image.h"
#include "Engine/Renderer/RenderSnapshot.h"
#include "Engine/Renderer/ViewportCapture.h"
#include "Support/AutomationTestClient.h"
#include "Support/EditorTestFixture.h"

#include <string>
#include <chrono>
#include <utility>

// editor.screenshot, and how the editor offers both screenshot methods (without a renderer, with failing captures, in the
// launcher state, in the Runtime subset), in process (Architecture §15.2) with CPU captures injected in place of the
// editor's GPU ones (ScreenshotCaptures). viewport.screenshot's own behaviour is tested with its shared handler in
// Tests/Source/Engine/Automation/Methods/ScreenshotMethodsTests.cpp; the real captures on the GPU (ImGuiScreenshotTests.cpp,
// the golden image "EditorDefaultLayout") and from Python (Tests/Automation/test_screenshot.py).

namespace Engine {

	namespace {

		// How often the injected captures ran.
		struct CaptureCounts
		{
			uint32_t ViewCalls = 0;
			uint32_t EditorUiCalls = 0;
			uint32_t FrameRequests = 0;
			uint64_t CompletedFrame = 0;
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
			specification.Screenshots.CompletedUiFrame = [record]()
			{
				return record->CompletedFrame;
			};
			specification.Screenshots.RequestUiFrame = [record]()
			{
				++record->FrameRequests;
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
				: m_Fixture(label), m_Counts(counts)
			{
				m_Fixture.CreateAndOpenProject();
				m_Fixture.CreateAndOpenScene();
				m_Client = CreateScope<Test::AutomationTestClient>(m_Fixture.GetEditor(), MakeScreenshotServerSpecification(counts));
			}

			[[nodiscard]] Result<Json> Call(std::string_view method, const Json& params)
			{
				if (method != "editor.screenshot")
					return m_Client->Call(method, params);
				const auto id = m_Client->Submit(method, params);
				auto& server = m_Client->GetServer();
				server.Pump();
				REQUIRE(server.TakeInProcessResponses(m_Client->GetClient()).empty());
				++m_Counts.CompletedFrame; // the injected host publishes a fresh UI frame between pumps
				server.Pump();
				auto responses = server.TakeInProcessResponses(m_Client->GetClient());
				REQUIRE(responses.size() == 1);
				CHECK(responses.front()["id"] == Json(id));
				REQUIRE(responses.front().contains("result"));
				return responses.front()["result"];
			}
		private:
			Test::EditorTestFixture m_Fixture;
			CaptureCounts& m_Counts; // test record outlives this fixture
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
			CHECK(counts.FrameRequests == 1);
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
			CHECK(counts.FrameRequests == 2);
			CHECK(counts.EditorUiCalls == 2);
		}

		TEST_CASE("ScreenshotMethods: each UI request waits for a newer completed frame and captures that frame")
		{
			Test::EditorTestFixture fixture("ScreenshotFreshFrames");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			CaptureCounts counts{ .CompletedFrame = 40 };
			auto specification = MakeScreenshotServerSpecification(counts);
			specification.Screenshots.EditorUi = [&counts]() -> Result<Image>
			{
				++counts.EditorUiCalls;
				ENGINE_TRY_ASSIGN(Image image, MakeBlankImage(1, 1));
				image.Pixels[0] = static_cast<std::byte>(counts.CompletedFrame);
				image.Pixels[3] = std::byte{ 255 };
				return image;
			};
			Test::AutomationTestClient client(fixture.GetEditor(), specification);
			auto& server = client.GetServer();
			for (uint32_t request = 1; request <= 2; ++request)
			{
				const auto id = client.Submit("editor.screenshot", Json::object());
				server.Pump();
				CHECK(counts.FrameRequests == request);
				CHECK(counts.EditorUiCalls == request - 1);
				CHECK(server.TakeInProcessResponses(client.GetClient()).empty());
				// A minimized host publishes no ordinary UI frames. Pumping requests alone must not reuse its old image.
				server.Pump();
				server.Pump();
				CHECK(server.TakeInProcessResponses(client.GetClient()).empty());
				CHECK(counts.EditorUiCalls == request - 1);
				++counts.CompletedFrame; // the host completes the explicitly requested offscreen frame
				server.Pump();
				const auto responses = server.TakeInProcessResponses(client.GetClient());
				REQUIRE(responses.size() == 1);
				CHECK(responses.front()["id"] == Json(id));
				REQUIRE(responses.front().contains("result"));
				const auto image = ReadPng(FileSystem::PathFromUtf8(ReadString(responses.front()["result"]["path"])));
				REQUIRE(image.has_value());
				CHECK(image->Pixels[0] == static_cast<std::byte>(counts.CompletedFrame));
				CHECK(counts.EditorUiCalls == request);
			}
		}

		TEST_CASE("ScreenshotMethods: disconnect cancels a pending UI capture before a frame is published")
		{
			Test::EditorTestFixture fixture("ScreenshotDisconnect");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			CaptureCounts counts;
			Test::AutomationTestClient client(fixture.GetEditor(), MakeScreenshotServerSpecification(counts));
			auto& server = client.GetServer();
			const ClientId owner = server.ConnectInProcess("screenshot-owner");
			server.SubmitInProcess(owner, RpcRequest{ .Id = Json(1), .IsNotification = false, .Method = "editor.screenshot", .Params = Json::object(), .TranscriptLine = std::nullopt });
			server.Pump();
			REQUIRE(counts.FrameRequests == 1);
			REQUIRE(server.TakeInProcessResponses(owner).empty());
			server.DisconnectInProcess(owner);
			++counts.CompletedFrame;
			server.Pump();
			CHECK(counts.EditorUiCalls == 0);

			const auto id = client.Submit("editor.screenshot", Json::object());
			server.Pump();
			CHECK(server.TakeInProcessResponses(client.GetClient()).empty());
			++counts.CompletedFrame;
			server.Pump();
			const auto responses = server.TakeInProcessResponses(client.GetClient());
			REQUIRE(responses.size() == 1);
			CHECK(responses.front()["id"] == Json(id));
			CHECK(responses.front().contains("result"));
			CHECK(counts.EditorUiCalls == 1);
			CHECK(counts.FrameRequests == 2);
		}

		TEST_CASE("ScreenshotMethods: a UI frame sequence reset cancels the pending screenshot")
		{
			Test::EditorTestFixture fixture("ScreenshotFrameReset");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			CaptureCounts counts{ .CompletedFrame = 10 };
			Test::AutomationTestClient client(fixture.GetEditor(), MakeScreenshotServerSpecification(counts));
			auto& server = client.GetServer();
			const auto id = client.Submit("editor.screenshot", Json::object());
			server.Pump();
			REQUIRE(server.TakeInProcessResponses(client.GetClient()).empty());
			counts.CompletedFrame = 0;
			server.Pump();
			const auto responses = server.TakeInProcessResponses(client.GetClient());
			REQUIRE(responses.size() == 1);
			CHECK(responses.front()["id"] == Json(id));
			CHECK(responses.front()["error"]["data"]["errorCode"] == Json("Cancelled"));
			CHECK(counts.EditorUiCalls == 0);
		}

		TEST_CASE("ScreenshotMethods: a stalled UI frame reaches its deadline without capturing a cached image")
		{
			Test::EditorTestFixture fixture("ScreenshotDeadline");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			CaptureCounts counts;
			auto now = std::chrono::steady_clock::time_point{};
			auto specification = MakeScreenshotServerSpecification(counts);
			specification.WallClock = [&now]()
			{
				return now;
			};
			Test::AutomationTestClient client(fixture.GetEditor(), specification);
			auto& server = client.GetServer();
			const auto* method = server.GetMethods().Find("editor.screenshot");
			REQUIRE(method != nullptr);
			const auto id = client.Submit("editor.screenshot", Json::object());
			server.Pump();
			REQUIRE(server.TakeInProcessResponses(client.GetClient()).empty());
			REQUIRE(method->Specification.TimeoutSeconds > 0);
			now += std::chrono::seconds(method->Specification.TimeoutSeconds) - std::chrono::milliseconds(1);
			server.Pump();
			CHECK(server.TakeInProcessResponses(client.GetClient()).empty());
			CHECK(counts.EditorUiCalls == 0);
			now += std::chrono::milliseconds(1);
			server.Pump();
			const auto responses = server.TakeInProcessResponses(client.GetClient());
			CHECK(counts.EditorUiCalls == 0);
			REQUIRE(responses.size() == 1);
			CHECK(responses.front()["id"] == Json(id));
			CHECK(responses.front()["error"]["data"]["errorCode"] == Json("Timeout"));
			++counts.CompletedFrame;
			server.Pump();
			CHECK(server.TakeInProcessResponses(client.GetClient()).empty());
			CHECK(counts.EditorUiCalls == 0);
			// A new request gets its own deadline and still requires another frame.
			const auto nextId = client.Submit("editor.screenshot", Json::object());
			server.Pump();
			REQUIRE(server.TakeInProcessResponses(client.GetClient()).empty());
			++counts.CompletedFrame;
			server.Pump();
			const auto completed = server.TakeInProcessResponses(client.GetClient());
			REQUIRE(completed.size() == 1);
			CHECK(completed.front()["id"] == Json(nextId));
			CHECK(completed.front().contains("result"));
			CHECK(counts.EditorUiCalls == 1);
		}

		TEST_CASE("ScreenshotMethods: missing fresh frame callbacks reject a UI request without using its capture")
		{
			Test::EditorTestFixture fixture("ScreenshotMissingFrameProvider");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			CaptureCounts counts;
			auto specification = MakeScreenshotServerSpecification(counts);
			SUBCASE("no completed frame counter")
			{
				specification.Screenshots.CompletedUiFrame = {};
			}
			SUBCASE("no frame request callback")
			{
				specification.Screenshots.RequestUiFrame = {};
			}
			Test::AutomationTestClient client(fixture.GetEditor(), specification);
			const auto shot = client.Call("editor.screenshot", Json::object());
			REQUIRE_FALSE(shot.has_value());
			CHECK(shot.error().GetCode() == ErrorCode::Unsupported);
			CHECK(counts.EditorUiCalls == 0);
			CHECK(counts.FrameRequests == 0);
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
			uint64_t completedFrame = 0;
			specification.Screenshots.CompletedUiFrame = [&completedFrame]()
			{
				return completedFrame;
			};
			specification.Screenshots.RequestUiFrame = []() {};
			Test::AutomationTestClient client(fixture.GetEditor(), specification);

			const Json viewport = client.Request("viewport.screenshot", Json{ { "view", "scene" } });
			REQUIRE(viewport.contains("error"));
			// A Gpu error is Internal on the wire (ADR 0008 decision 5).
			CHECK(viewport["error"]["data"]["errorCode"] == Json("Gpu"));
			CHECK(viewport["error"]["data"].dump().contains("while rendering the viewport"));

			const auto id = client.Submit("editor.screenshot", Json::object());
			client.GetServer().Pump();
			CHECK(client.GetServer().TakeInProcessResponses(client.GetClient()).empty());
			++completedFrame;
			client.GetServer().Pump();
			const auto responses = client.GetServer().TakeInProcessResponses(client.GetClient());
			REQUIRE(responses.size() == 1);
			CHECK(responses.front()["id"] == Json(id));
			CHECK(responses.front()["error"]["data"]["errorCode"] == Json("InvalidState"));
			CHECK(responses.front()["error"]["data"].dump().contains("while rendering the editor UI"));
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
			CHECK(editor->Pending);
			CHECK_FALSE(editor->Specification.SupportsDryRun);
			CHECK_FALSE(editor->Specification.AllowedInBatch);
		}
	}

}
