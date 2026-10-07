#include "TestsPCH.h"

#include "EditorCore/Automation/ScreenshotMethods.h"

#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Graphics/Image.h"
#include "Engine/Renderer/ViewportCapture.h"
#include "Support/AutomationTestClient.h"
#include "Support/EditorTestFixture.h"

// viewport.screenshot and editor.screenshot in process (Architecture §15.2), with CPU captures injected in place of the
// editor's GPU ones (ScreenshotCaptures), so the methods' params, results, files and errors are tested without a device.
// The real captures are tested on the GPU (ViewportCaptureTests.cpp, ImGuiScreenshotTests.cpp), through the Editor process
// (the golden image "ImGuiDemo") and from Python (Tests/Automation/test_screenshot.py).

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
			uint32_t ViewportCalls = 0;
			uint32_t LastWidth = 0;
			uint32_t LastHeight = 0;
			uint32_t EditorUiCalls = 0;
		};

		// Captures that render the pattern (the viewport at the requested size, the UI at 1600x900, the headless editor's
		// window size) and count their calls in `log`, which outlives the server.
		ScreenshotCaptures MakeCaptures(CaptureLog& log)
		{
			CaptureLog* record = &log;
			ScreenshotCaptures captures;
			captures.Viewport = [record](uint32_t width, uint32_t height) -> Result<Image>
			{
				++record->ViewportCalls;
				record->LastWidth = width;
				record->LastHeight = height;
				return MakePatternImage(width, height);
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

		// The pointer of the error's first issue.
		std::string FirstIssuePointer(const Error& error)
		{
			REQUIRE_FALSE(error.GetIssues().empty());
			return error.GetIssues().front().JsonPointer;
		}

	}

	TEST_SUITE("EditorCore")
	{
		TEST_CASE("ScreenshotMethods: viewport.screenshot renders through the injected capture and writes the PNG")
		{
			Test::EditorTestFixture fixture("ViewportScreenshot");
			fixture.CreateAndOpenProject();
			CaptureLog log;
			Test::AutomationTestClient client(fixture.GetEditor(), MakeScreenshotServerSpecification(log));

			const Result<Json> shot = client.Call("viewport.screenshot", Json{ { "view", "scene" }, { "width", 320 }, { "height", 200 }, { "maxDimension", 4096 } });
			REQUIRE_MESSAGE(shot.has_value(), shot.error().ToString());
			CHECK(log.ViewportCalls == 1);
			CHECK(log.LastWidth == 320);
			CHECK(log.LastHeight == 200);
			CHECK((*shot)["view"] == Json("Scene"));
			CHECK((*shot)["mimeType"] == Json("image/png"));
			CHECK(ReadUnsigned((*shot)["width"]) == 320);
			CHECK(ReadUnsigned((*shot)["height"]) == 200);
			CHECK(ReadString((*shot)["data"]).empty());

			// The PNG lies in the project's output directory and holds exactly the captured pixels.
			const std::string path = ReadString((*shot)["path"]);
			CHECK(path.contains("Library/Automation/Out/"));
			CHECK(path.ends_with(".png"));
			const Result<Image> written = ReadPng(FileSystem::PathFromUtf8(path));
			REQUIRE_MESSAGE(written.has_value(), written.error().ToString());
			CHECK(written->Pixels == MakePatternImage(320, 200).Pixels);

			// The defaults: the golden-image size, downscaled to 1024 at most (it already fits).
			const Result<Json> defaults = client.Call("viewport.screenshot", Json{ { "view", "Scene" } });
			REQUIRE_MESSAGE(defaults.has_value(), defaults.error().ToString());
			CHECK(log.LastWidth == DefaultViewportScreenshotWidth);
			CHECK(log.LastHeight == DefaultViewportScreenshotHeight);
			CHECK(ReadString((*defaults)["path"]) != path);
		}

		TEST_CASE("ScreenshotMethods: maxDimension downscales the PNG and inline returns it as base64")
		{
			Test::EditorTestFixture fixture("ViewportScreenshotInline");
			fixture.CreateAndOpenProject();
			CaptureLog log;
			Test::AutomationTestClient client(fixture.GetEditor(), MakeScreenshotServerSpecification(log));

			const Result<Json> shot =
				client.Call("viewport.screenshot", Json{ { "view", "SCENE" }, { "width", 640 }, { "height", 360 }, { "maxDimension", 320 }, { "inline", true } });
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
			AutomationServerSpecification specification = Test::MakeTestServerSpecification();
			specification.RendererName = "vulkan";
			specification.Screenshots.Viewport = [](uint32_t width, uint32_t height) -> Result<Image>
			{
				return MakeNoiseImage(width, height);
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
			Test::EditorTestFixture fixture("ViewportScreenshotLater");
			fixture.CreateAndOpenProject();
			CaptureLog log;
			Test::AutomationTestClient client(fixture.GetEditor(), MakeScreenshotServerSpecification(log));

			const std::vector<std::pair<Json, std::string>> refused = {
				{ Json{ { "view", "game" } }, "/view" },
				{ Json{ { "view", "scene" }, { "camera", "/Main Camera" } }, "/camera" },
				{ Json{ { "view", "scene" }, { "camera", "" } }, "/camera" },
				{ Json{ { "view", "scene" }, { "debugView", "Albedo" } }, "/debugView" },
				{ Json{ { "view", "scene" }, { "annotate", Json{ { "labels", "all" } } } }, "/annotate" },
			};
			for (const auto& [params, pointer] : refused)
			{
				INFO(params.dump());
				const Result<Json> shot = client.Call("viewport.screenshot", params);
				REQUIRE_FALSE(shot.has_value());
				CHECK(shot.error().GetCode() == ErrorCode::Unsupported);
				CHECK(FirstIssuePointer(shot.error()) == pointer);
			}
			// Refused before anything renders.
			CHECK(log.ViewportCalls == 0);
		}

		TEST_CASE("ScreenshotMethods: a missing view and sizes out of range are InvalidArgument")
		{
			Test::EditorTestFixture fixture("ViewportScreenshotParams");
			fixture.CreateAndOpenProject();
			CaptureLog log;
			Test::AutomationTestClient client(fixture.GetEditor(), MakeScreenshotServerSpecification(log));

			const std::vector<Json> invalid = {
				Json::object(),
				Json{ { "view", "scene" }, { "width", 0 } },
				Json{ { "view", "scene" }, { "height", MaxViewportScreenshotDimension + 1 } },
				Json{ { "view", "scene" }, { "maxDimension", 0 } },
				Json{ { "view", "top" } },
			};
			for (const Json& params : invalid)
			{
				INFO(params.dump());
				const Result<Json> shot = client.Call("viewport.screenshot", params);
				REQUIRE_FALSE(shot.has_value());
				CHECK(shot.error().GetCode() == ErrorCode::InvalidArgument);
			}
			const Result<Json> editor = client.Call("editor.screenshot", Json{ { "maxDimension", MaxViewportScreenshotDimension + 1 } });
			REQUIRE_FALSE(editor.has_value());
			CHECK(editor.error().GetCode() == ErrorCode::InvalidArgument);
			CHECK(log.ViewportCalls == 0);
			CHECK(log.EditorUiCalls == 0);
		}

		TEST_CASE("ScreenshotMethods: editor.screenshot renders the editor UI capture, downscaled to maxDimension")
		{
			Test::EditorTestFixture fixture("EditorScreenshot");
			fixture.CreateAndOpenProject();
			CaptureLog log;
			Test::AutomationTestClient client(fixture.GetEditor(), MakeScreenshotServerSpecification(log));

			const Result<Json> shot = client.Call("editor.screenshot", Json::object());
			REQUIRE_MESSAGE(shot.has_value(), shot.error().ToString());
			CHECK(log.EditorUiCalls == 1);
			CHECK((*shot)["mimeType"] == Json("image/png"));
			CHECK(ReadUnsigned((*shot)["width"]) == 1024);
			CHECK(ReadUnsigned((*shot)["height"]) == 576);
			const Result<Image> downscaled = ReadPng(FileSystem::PathFromUtf8(ReadString((*shot)["path"])));
			REQUIRE(downscaled.has_value());
			CHECK(downscaled->Width == 1024);

			const Result<Json> full = client.Call("editor.screenshot", Json{ { "maxDimension", 2000 } });
			REQUIRE_MESSAGE(full.has_value(), full.error().ToString());
			CHECK(ReadUnsigned((*full)["width"]) == 1600);
			CHECK(ReadUnsigned((*full)["height"]) == 900);
		}

		TEST_CASE("ScreenshotMethods: without captures (--renderer none) both methods are Unsupported")
		{
			Test::AutomationFixture setup("ScreenshotNoRenderer", false);
			for (const auto& [method, params] : { std::pair{ "viewport.screenshot", Json{ { "view", "scene" } } }, std::pair{ "editor.screenshot", Json::object() } })
			{
				INFO(method);
				const Result<Json> shot = setup.Call(method, params);
				REQUIRE_FALSE(shot.has_value());
				CHECK(shot.error().GetCode() == ErrorCode::Unsupported);
				CHECK(shot.error().GetMessageText().contains("--renderer none"));
			}
		}

		TEST_CASE("ScreenshotMethods: a failed capture is the method's error, with what was rendering as context")
		{
			Test::EditorTestFixture fixture("ScreenshotFailure");
			fixture.CreateAndOpenProject();
			AutomationServerSpecification specification = Test::MakeTestServerSpecification();
			specification.Screenshots.Viewport = [](uint32_t /*width*/, uint32_t /*height*/) -> Result<Image>
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
			CaptureLog log;
			Test::AutomationTestClient client(fixture.GetEditor(), MakeScreenshotServerSpecification(log));
			for (const auto& [method, params] : { std::pair{ "viewport.screenshot", Json{ { "view", "scene" } } }, std::pair{ "editor.screenshot", Json::object() } })
			{
				INFO(method);
				const Result<Json> shot = client.Call(method, params);
				REQUIRE_FALSE(shot.has_value());
				CHECK(shot.error().GetCode() == ErrorCode::InvalidState);
			}
			CHECK(log.ViewportCalls == 0);
			CHECK(log.EditorUiCalls == 0);
		}
	}

}
