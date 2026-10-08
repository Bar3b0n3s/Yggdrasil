#include "TestsPCH.h"

#include "Engine/Automation/Methods/ScreenshotMethods.h"

#include "EditorCore/Automation/AutomationServer.h"
#include "EditorCore/Automation/ScreenshotMethods.h"
#include "EditorCore/EditorContext.h"
#include "Engine/Asset/BuiltinAssets.h"
#include "Engine/Automation/Protocol/MethodRegistry.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Graphics/Image.h"
#include "Engine/Renderer/RenderSnapshot.h"
#include "Engine/Renderer/ViewportCapture.h"
#include "Engine/Scene/RenderExtraction.h"
#include "Support/AutomationTestClient.h"
#include "Support/EditorTestFixture.h"
#include "Support/GlmApprox.h"

#include <format>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// viewport.screenshot, the handler the Editor and the Runtime share, in process (Architecture §15.2) through the editor's
// server, with CPU captures injected in place of the editor's GPU ones (ScreenshotCaptures), so the method's params, views,
// cameras, results, files and errors are tested without a device. editor.screenshot and how the editor offers both
// screenshot methods are tested in Tests/Source/EditorCore/Automation/ScreenshotMethodsTests.cpp. The real captures are tested on the GPU
// (ViewportCaptureTests.cpp, ImGuiScreenshotTests.cpp, the golden images "LitScene" and "ImGuiDemo") and from Python
// (Tests/Automation/test_screenshot.py, test_game_view.py).

namespace Engine {

	namespace {

		// An RGBA8 image whose pixels encode their position, so a downscale or a mix-up of rows shows.
		Image MakePatternImage(uint32_t width, uint32_t height)
		{
			Result<Image> image = CreateImage(width, height, nvrhi::Format::RGBA8_UNORM);
			REQUIRE_MESSAGE(image.has_value(), image.error().ToString());
			for (uint32_t y = 0; y < height; ++y)
			{
				for (uint32_t x = 0; x < width; ++x)
				{
					const size_t offset = (static_cast<size_t>(y) * width + x) * 4;
					image->Pixels[offset + 0] = static_cast<std::byte>(x & 0xFFU);
					image->Pixels[offset + 1] = static_cast<std::byte>(y & 0xFFU);
					image->Pixels[offset + 2] = static_cast<std::byte>((x + y) & 0xFFU);
					image->Pixels[offset + 3] = std::byte{ 0xFF };
				}
			}
			return std::move(*image);
		}

		// An RGBA8 image of hashed noise, which deflate cannot compress: its PNG is about as large as its pixels.
		Image MakeNoiseImage(uint32_t width, uint32_t height)
		{
			Result<Image> image = CreateImage(width, height, nvrhi::Format::RGBA8_UNORM);
			REQUIRE_MESSAGE(image.has_value(), image.error().ToString());
			for (size_t index = 0; index < image->Pixels.size(); ++index)
			{
				uint32_t hash = static_cast<uint32_t>(index) * 0x9E3779B9U;
				hash ^= hash >> 16U;
				hash *= 0x85EBCA6BU;
				hash ^= hash >> 13U;
				image->Pixels[index] = static_cast<std::byte>(hash & 0xFFU);
			}
			return std::move(*image);
		}

		// What the injected captures saw.
		struct CaptureLog
		{
			uint32_t ViewCalls = 0;
			uint32_t LastWidth = 0;
			uint32_t LastHeight = 0;
			std::optional<RenderSnapshot> LastSnapshot{};
			uint32_t EditorUiCalls = 0;
		};

		// Captures that render the pattern (the view at the requested size, the UI at 1600x900, the headless editor's window
		// size) and record their calls in `log`, which outlives the server.
		ScreenshotCaptures MakeCaptures(CaptureLog& log)
		{
			CaptureLog* record = &log;
			ScreenshotCaptures captures;
			captures.View = [record](const RenderSnapshot& snapshot, const ViewportScreenshotRequest& request) -> Result<Image>
			{
				++record->ViewCalls;
				record->LastWidth = request.Width;
				record->LastHeight = request.Height;
				record->LastSnapshot = snapshot;
				return MakePatternImage(request.Width, request.Height);
			};
			captures.EditorUi = [record]() -> Result<Image>
			{
				++record->EditorUiCalls;
				return MakePatternImage(1600, 900);
			};
			return captures;
		}

		AutomationServerSpecification MakeScreenshotServerSpecification(CaptureLog& log)
		{
			AutomationServerSpecification specification = Test::MakeTestServerSpecification();
			specification.RendererName = "vulkan";
			specification.Screenshots = MakeCaptures(log);
			return specification;
		}

		// RFC 4648 base64 (with padding) decoded, for the inline data.
		std::vector<std::byte> DecodeBase64(std::string_view text)
		{
			constexpr std::string_view Alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
			REQUIRE(text.size() % 4 == 0);
			std::vector<std::byte> bytes;
			uint32_t group = 0;
			int bits = 0;
			for (const char character : text)
			{
				if (character == '=')
					break;
				const size_t value = Alphabet.find(character);
				REQUIRE(value != std::string_view::npos);
				group = (group << 6U) | static_cast<uint32_t>(value);
				bits += 6;
				if (bits >= 8)
				{
					bits -= 8;
					bytes.push_back(static_cast<std::byte>((group >> static_cast<uint32_t>(bits)) & 0xFFU));
				}
			}
			return bytes;
		}

		std::string ReadString(const Json& value)
		{
			return JsonReader(value).ReadString().value_or(std::string());
		}

		uint32_t ReadUnsigned(const Json& value)
		{
			return JsonReader(value).ReadUInt32().value_or(0);
		}

		Json ParseJson(std::string_view text)
		{
			Result<Json> json = JsonReader::Parse(text);
			REQUIRE_MESSAGE(json.has_value(), json.error().ToString());
			return std::move(*json);
		}

		// The pointer of the error's first issue. An error without issues gives a text that is no pointer and names the whole
		// error, so the comparison that called this fails showing it (a REQUIRE here would throw out of that CHECK).
		std::string FirstIssuePointer(const Error& error)
		{
			if (error.GetIssues().empty())
				return std::format("(an error without issues: {})", error.ToString());
			return error.GetIssues().front().JsonPointer;
		}

		// A project with the scene Assets/Scenes/Main.scene open, and a client whose server has the CPU captures of `log`.
		class ScreenshotSetup
		{
		public:
			ScreenshotSetup(std::string_view label, CaptureLog& log)
				: m_Fixture(label)
			{
				m_Fixture.CreateAndOpenProject();
				m_Fixture.CreateAndOpenScene();
				m_Client = CreateScope<Test::AutomationTestClient>(m_Fixture.GetEditor(), MakeScreenshotServerSpecification(log));
			}

			[[nodiscard]] Test::EditorTestFixture& GetFixture() { return m_Fixture; }
			[[nodiscard]] Result<Json> Call(std::string_view method, const Json& params) { return m_Client->Call(method, params); }
		private:
			Test::EditorTestFixture m_Fixture;
			Scope<Test::AutomationTestClient> m_Client;
		};

	}

	TEST_SUITE("Automation")
	{
		TEST_CASE("ScreenshotMethods: viewport.screenshot renders the scene view through the injected capture and writes the PNG")
		{
			CaptureLog log;
			ScreenshotSetup setup("ViewportScreenshot", log);

			const Result<Json> shot = setup.Call("viewport.screenshot", Json{ { "view", "scene" }, { "width", 320 }, { "height", 200 }, { "maxDimension", 4096 } });
			REQUIRE_MESSAGE(shot.has_value(), shot.error().ToString());
			CHECK(log.ViewCalls == 1);
			CHECK(log.LastWidth == 320);
			CHECK(log.LastHeight == 200);
			CHECK((*shot)["view"] == Json("Scene"));
			CHECK((*shot)["target"] == Json("Edit"));
			CHECK(ReadString((*shot)["camera"]["id"]).empty());
			CHECK((*shot)["mimeType"] == Json("image/png"));
			CHECK(ReadUnsigned((*shot)["width"]) == 320);
			CHECK(ReadUnsigned((*shot)["height"]) == 200);
			CHECK(ReadString((*shot)["data"]).empty());

			// The scene view renders through the editor's scene-view camera, extracted for the requested size, at alpha 1.
			REQUIRE(log.LastSnapshot.has_value());
			CHECK(log.LastSnapshot->HasCamera);
			CHECK_FALSE(log.LastSnapshot->Camera.Entity.IsValid());
			CHECK(Test::ApproxEqual(log.LastSnapshot->Camera.Position, setup.GetFixture().GetEditor().GetSceneViewCamera().Position, 1e-6f));
			CHECK(log.LastSnapshot->Camera.ViewportWidth == 320);
			CHECK(log.LastSnapshot->Camera.ViewportHeight == 200);
			CHECK(log.LastSnapshot->Alpha == 1.0f);

			// The PNG lies in the project's output directory and holds exactly the captured pixels.
			const std::string path = ReadString((*shot)["path"]);
			CHECK(path.contains("Library/Automation/Out/"));
			CHECK(path.ends_with(".png"));
			const Result<Image> written = ReadPng(FileSystem::PathFromUtf8(path));
			REQUIRE_MESSAGE(written.has_value(), written.error().ToString());
			CHECK(written->Pixels == MakePatternImage(320, 200).Pixels);

			// The defaults: the golden-image size, downscaled to 1024 at most (it already fits).
			const Result<Json> defaults = setup.Call("viewport.screenshot", Json{ { "view", "Scene" } });
			REQUIRE_MESSAGE(defaults.has_value(), defaults.error().ToString());
			CHECK(log.LastWidth == DefaultViewportScreenshotWidth);
			CHECK(log.LastHeight == DefaultViewportScreenshotHeight);
			CHECK(ReadString((*defaults)["path"]) != path);
		}

		TEST_CASE("ScreenshotMethods: the scene view shows the edit scene's current transforms and meshes")
		{
			CaptureLog log;
			ScreenshotSetup setup("ViewportScreenshotScene", log);
			REQUIRE(setup.Call("entity.create", ParseJson(R"({"name":"Cube","components":{"MeshRenderer":{"Mesh":"engine://Meshes/Cube"}}})")).has_value());
			REQUIRE(setup.Call("entity.update", ParseJson(R"({"entity":"/Cube","components":{"Transform":{"Translation":[3,0,0]}}})")).has_value());
			const uint64_t revision = setup.GetFixture().GetEditor().GetScene().GetRevision();

			REQUIRE(setup.Call("viewport.screenshot", Json{ { "view", "scene" } }).has_value());
			REQUIRE(log.LastSnapshot.has_value());
			REQUIRE(log.LastSnapshot->Meshes.size() == 1);
			CHECK(log.LastSnapshot->Meshes[0].Mesh == BuiltinAssetHandles::CubeMesh);
			CHECK(log.LastSnapshot->Meshes[0].World[3].x == doctest::Approx(3.0f));

			// A later write shows in the next screenshot, and rendering leaves the scene's revision alone.
			REQUIRE(setup.Call("entity.update", ParseJson(R"({"entity":"/Cube","components":{"Transform":{"Translation":[-2,0,0]}}})")).has_value());
			const uint64_t moved = setup.GetFixture().GetEditor().GetScene().GetRevision();
			REQUIRE(setup.Call("viewport.screenshot", Json{ { "view", "scene" } }).has_value());
			CHECK(log.LastSnapshot->Meshes[0].World[3].x == doctest::Approx(-2.0f));
			CHECK(moved > revision);
			CHECK(setup.GetFixture().GetEditor().GetScene().GetRevision() == moved);
		}

		TEST_CASE("ScreenshotMethods: the game view renders the primary camera, and a scene without one is InvalidState")
		{
			CaptureLog log;
			ScreenshotSetup setup("ViewportScreenshotGame", log);

			// No primary camera: InvalidState naming the validator's code, before anything renders.
			const Result<Json> none = setup.Call("viewport.screenshot", Json{ { "view", "game" } });
			REQUIRE_FALSE(none.has_value());
			CHECK(none.error().GetCode() == ErrorCode::InvalidState);
			CHECK(none.error().GetMessageText().contains("SCENE_NO_PRIMARY_CAMERA"));
			CHECK(log.ViewCalls == 0);

			const Result<Json> created = setup.Call("entity.create",
				ParseJson(R"({"name":"Camera","components":{"Transform":{"Translation":[0,1,4]},"Camera":{"Primary":true}}})"));
			REQUIRE_MESSAGE(created.has_value(), created.error().ToString());
			const Result<Json> shot = setup.Call("viewport.screenshot", Json{ { "view", "game" }, { "width", 160 }, { "height", 90 } });
			REQUIRE_MESSAGE(shot.has_value(), shot.error().ToString());
			CHECK((*shot)["view"] == Json("Game"));
			CHECK((*shot)["target"] == Json("Edit"));
			CHECK((*shot)["camera"]["path"] == Json("/Camera"));
			CHECK((*shot)["camera"]["name"] == Json("Camera"));
			CHECK((*shot)["camera"]["id"] == (*created)["entity"]["id"]);
			REQUIRE(log.LastSnapshot.has_value());
			CHECK(log.LastSnapshot->HasCamera);
			CHECK(log.LastSnapshot->Camera.ViewportWidth == 160);
			CHECK(Test::ApproxEqual(log.LastSnapshot->Camera.Position, glm::vec3(0.0f, 1.0f, 4.0f), 1e-6f));
		}

		TEST_CASE("ScreenshotMethods: camera renders through a camera entity and refuses other references at /camera")
		{
			CaptureLog log;
			ScreenshotSetup setup("ViewportScreenshotCamera", log);
			REQUIRE(setup.Call("entity.create", ParseJson(R"({"name":"Side","components":{"Transform":{"Translation":[5,1,0]},"Camera":{}}})")).has_value());
			REQUIRE(setup.Call("entity.create", ParseJson(R"({"name":"Cube"})")).has_value());

			// Any view takes "camera"; the scene view through a camera entity reports that camera.
			const Result<Json> side = setup.Call("viewport.screenshot", Json{ { "view", "scene" }, { "camera", "/Side" }, { "width", 64 }, { "height", 64 } });
			REQUIRE_MESSAGE(side.has_value(), side.error().ToString());
			CHECK((*side)["camera"]["path"] == Json("/Side"));
			REQUIRE(log.LastSnapshot.has_value());
			CHECK(Test::ApproxEqual(log.LastSnapshot->Camera.Position, glm::vec3(5.0f, 1.0f, 0.0f), 1e-6f));

			const std::vector<std::pair<std::string, ErrorCode>> refused = {
				{ "/Cube", ErrorCode::InvalidArgument },
				{ "/Nobody", ErrorCode::NotFound },
				{ "", ErrorCode::InvalidArgument },
				{ "not a reference", ErrorCode::InvalidArgument },
			};
			const uint32_t calls = log.ViewCalls;
			for (const auto& [camera, code] : refused)
			{
				INFO(camera);
				const Result<Json> shot = setup.Call("viewport.screenshot", Json{ { "view", "game" }, { "camera", camera } });
				REQUIRE_FALSE(shot.has_value());
				CHECK(shot.error().GetCode() == code);
				CHECK(FirstIssuePointer(shot.error()) == "/camera");
			}
			CHECK(log.ViewCalls == calls);
		}

		TEST_CASE("ScreenshotMethods: the play target needs a play session")
		{
			CaptureLog log;
			ScreenshotSetup setup("ViewportScreenshotTarget", log);
			const Result<Json> play = setup.Call("viewport.screenshot", Json{ { "view", "scene" }, { "target", "play" } });
			REQUIRE_FALSE(play.has_value());
			CHECK(play.error().GetCode() == ErrorCode::InvalidState);
			CHECK(FirstIssuePointer(play.error()) == "/target");
			const Result<Json> edit = setup.Call("viewport.screenshot", Json{ { "view", "scene" }, { "target", "edit" } });
			REQUIRE_MESSAGE(edit.has_value(), edit.error().ToString());
			CHECK((*edit)["target"] == Json("Edit"));
		}

		TEST_CASE("ScreenshotMethods: maxDimension downscales the PNG and inline returns it as base64")
		{
			CaptureLog log;
			ScreenshotSetup setup("ViewportScreenshotInline", log);

			const Result<Json> shot =
				setup.Call("viewport.screenshot", Json{ { "view", "SCENE" }, { "width", 640 }, { "height", 360 }, { "maxDimension", 320 }, { "inline", true } });
			REQUIRE_MESSAGE(shot.has_value(), shot.error().ToString());
			CHECK(log.LastWidth == 640);
			CHECK((*shot)["view"] == Json("Scene"));
			CHECK(ReadUnsigned((*shot)["width"]) == 320);
			CHECK(ReadUnsigned((*shot)["height"]) == 180);
			const Result<Buffer> file = FileSystem::ReadFile(FileSystem::PathFromUtf8(ReadString((*shot)["path"])));
			REQUIRE(file.has_value());
			CHECK(DecodeBase64(ReadString((*shot)["data"])) == *file);
			CHECK((*shot)["inlineOmitted"] == Json(false));
		}

		TEST_CASE("ScreenshotMethods: inline leaves out a PNG over the inline budget, so the result is never offloaded")
		{
			Test::EditorTestFixture fixture("ViewportScreenshotLargeInline");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			AutomationServerSpecification specification = Test::MakeTestServerSpecification();
			specification.RendererName = "vulkan";
			specification.Screenshots.View = [](const RenderSnapshot& /*snapshot*/, const ViewportScreenshotRequest& request) -> Result<Image>
			{
				return MakeNoiseImage(request.Width, request.Height);
			};
			// The client offloads results over the threshold, as the TCP clients (the MCP bridge) do.
			Test::AutomationTestClient client(fixture.GetEditor(), specification);

			// 256x256 pixels of noise: a PNG of about 256 KB, whose base64 alone would be far over the 48 KB threshold.
			const Result<Json> large =
				client.Call("viewport.screenshot", Json{ { "view", "scene" }, { "width", 256 }, { "height", 256 }, { "inline", true } });
			REQUIRE_MESSAGE(large.has_value(), large.error().ToString());
			INFO(large->dump().substr(0, 1024));
			REQUIRE_FALSE(large->contains("truncated"));
			CHECK((*large)["mimeType"] == Json("image/png"));
			CHECK((*large)["inlineOmitted"] == Json(true));
			CHECK(ReadString((*large)["data"]).empty());
			const Result<Buffer> largeFile = FileSystem::ReadFile(FileSystem::PathFromUtf8(ReadString((*large)["path"])));
			REQUIRE(largeFile.has_value());
			CHECK(largeFile->size() > MaxInlineScreenshotPngBytes);
			const Result<Image> written = DecodePng(*largeFile);
			REQUIRE_MESSAGE(written.has_value(), written.error().ToString());
			CHECK(written->Pixels == MakeNoiseImage(256, 256).Pixels);

			// A PNG within the budget is returned inline.
			const Result<Json> fitting =
				client.Call("viewport.screenshot", Json{ { "view", "scene" }, { "width", 32 }, { "height", 32 }, { "inline", true } });
			REQUIRE_MESSAGE(fitting.has_value(), fitting.error().ToString());
			CHECK((*fitting)["inlineOmitted"] == Json(false));
			const Result<Buffer> fittingFile = FileSystem::ReadFile(FileSystem::PathFromUtf8(ReadString((*fitting)["path"])));
			REQUIRE(fittingFile.has_value());
			CHECK(fittingFile->size() <= MaxInlineScreenshotPngBytes);
			CHECK(DecodeBase64(ReadString((*fitting)["data"])) == *fittingFile);

			// Without inline nothing is left out.
			const Result<Json> plain = client.Call("viewport.screenshot", Json{ { "view", "scene" }, { "width", 256 }, { "height", 256 } });
			REQUIRE_MESSAGE(plain.has_value(), plain.error().ToString());
			CHECK((*plain)["inlineOmitted"] == Json(false));
			CHECK(ReadString((*plain)["data"]).empty());
		}

		TEST_CASE("ScreenshotMethods: members that need later milestones are Unsupported at their pointer")
		{
			CaptureLog log;
			ScreenshotSetup setup("ViewportScreenshotLater", log);

			// M9's debug views (AO, ShadowCascades, Overdraw) and annotations (Docs/Decisions/0013-m8-decisions.md decision 12).
			const std::vector<std::pair<Json, std::string>> refused = {
				{ Json{ { "view", "scene" }, { "debugView", "AO" } }, "/debugView" },
				{ Json{ { "view", "game" }, { "debugView", "ShadowCascades" } }, "/debugView" },
				{ Json{ { "view", "scene" }, { "debugView", "Overdraw" } }, "/debugView" },
				{ Json{ { "view", "scene" }, { "annotate", Json{ { "labels", "all" } } } }, "/annotate" },
			};
			for (const auto& [params, pointer] : refused)
			{
				INFO(params.dump());
				const Result<Json> shot = setup.Call("viewport.screenshot", params);
				REQUIRE_FALSE(shot.has_value());
				CHECK(shot.error().GetCode() == ErrorCode::Unsupported);
				CHECK(FirstIssuePointer(shot.error()) == pointer);
			}
			// Refused before anything renders.
			CHECK(log.ViewCalls == 0);
		}

		TEST_CASE("ScreenshotMethods: debugView renders the named debug view, ignoring case, and empty is Lit")
		{
			// M8 (§8.5; ADR 0009 decision 33 deferred it here; Docs/Decisions/0013-m8-decisions.md decision 12).
			CaptureLog log;
			ScreenshotSetup setup("ViewportScreenshotDebugView", log);
			const std::vector<std::pair<std::string, RenderDebugView>> views = {
				{ "Lit", RenderDebugView::Lit },
				{ "albedo", RenderDebugView::Albedo },
				{ "NORMALS", RenderDebugView::Normals },
				{ "Roughness", RenderDebugView::Roughness },
				{ "Metallic", RenderDebugView::Metallic },
				{ "Emissive", RenderDebugView::Emissive },
				{ "", RenderDebugView::Lit },
			};
			for (const auto& [name, view] : views)
			{
				INFO(name);
				const Result<Json> shot = setup.Call("viewport.screenshot", Json{ { "view", "scene" }, { "debugView", name } });
				REQUIRE_MESSAGE(shot.has_value(), shot.error().ToString());
				REQUIRE(log.LastSnapshot.has_value());
				CHECK(log.LastSnapshot->DebugView == view);
			}
			// Without the member the view is Lit.
			REQUIRE(setup.Call("viewport.screenshot", Json{ { "view", "scene" } }).has_value());
			CHECK(log.LastSnapshot->DebugView == RenderDebugView::Lit);
		}

		TEST_CASE("ScreenshotMethods: an unknown debug view is InvalidArgument at /debugView naming the valid views")
		{
			CaptureLog log;
			ScreenshotSetup setup("ViewportScreenshotBadDebugView", log);
			const Result<Json> shot = setup.Call("viewport.screenshot", Json{ { "view", "scene" }, { "debugView", "Wireframe" } });
			REQUIRE_FALSE(shot.has_value());
			CHECK(shot.error().GetCode() == ErrorCode::InvalidArgument);
			CHECK(FirstIssuePointer(shot.error()) == "/debugView");
			CHECK(shot.error().ToString().contains("Albedo"));
			CHECK(log.ViewCalls == 0);
		}

		TEST_CASE("ScreenshotMethods: a missing view and sizes out of range are InvalidArgument")
		{
			CaptureLog log;
			ScreenshotSetup setup("ViewportScreenshotParams", log);

			const std::vector<Json> invalid = {
				Json::object(),
				Json{ { "view", "scene" }, { "width", 0 } },
				Json{ { "view", "scene" }, { "height", MaxViewportScreenshotDimension + 1 } },
				Json{ { "view", "scene" }, { "maxDimension", 0 } },
				Json{ { "view", "top" } },
				Json{ { "view", "scene" }, { "target", "both" } },
			};
			for (const Json& params : invalid)
			{
				INFO(params.dump());
				const Result<Json> shot = setup.Call("viewport.screenshot", params);
				REQUIRE_FALSE(shot.has_value());
				CHECK(shot.error().GetCode() == ErrorCode::InvalidArgument);
			}
			const Result<Json> editor = setup.Call("editor.screenshot", Json{ { "maxDimension", MaxViewportScreenshotDimension + 1 } });
			REQUIRE_FALSE(editor.has_value());
			CHECK(editor.error().GetCode() == ErrorCode::InvalidArgument);
			CHECK(log.ViewCalls == 0);
			CHECK(log.EditorUiCalls == 0);
		}
	}

}
