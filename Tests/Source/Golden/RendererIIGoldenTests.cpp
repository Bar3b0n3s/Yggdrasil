#include "TestsPCH.h"

#include "EditorCore/Automation/BatchRunner.h"
#include "Engine/AssetPipeline/EditorAssetManager.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Renderer/ShadowCascades.h"
#include "Support/AutomationTestClient.h"
#include "Support/ExpectLog.h"
#include "Support/GoldenImage.h"
#include "Support/GoldenScene.h"
#include "Support/HeadlessGpuFixture.h"
#include "Support/TestData.h"
#include "Support/TestOptions.h"
#include "Support/Utf8Path.h"
#include "Support/WaitUntil.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace Engine {

	namespace {

		constexpr uint32_t RendererIIGoldenWidth = 640;
		constexpr uint32_t RendererIIGoldenHeight = 360;
		// Pinned by the scaffold replay below, including its fixed EditorTestIdState and canonical entity order.
		constexpr UUID RendererIIOutlineCube{ 0x9fccff8e11f046df };
		constexpr UUID RendererIIOutlineSphere{ 0x3c34bab161152848 };
		constexpr UUID RendererIIAnnotatedCube{ 0x6f73764e2c7305bf };
		constexpr UUID RendererIIAnnotatedSphere{ 0xd2f498918b91281f };

		class RendererIIGoldenFixture
		{
		public:
			RendererIIGoldenFixture(Test::HeadlessGpuFixture& gpu, std::string_view name)
				: m_Gpu(gpu), m_Name(name)
			{
			}

			[[nodiscard]] Image Capture(Test::GoldenSceneOptions options = {})
			{
				options.EditSnapshot = [edit = options.EditSnapshot](RenderSnapshot& snapshot)
				{
					CHECK(snapshot.Post.FxaaEnabled);
					snapshot.Quality.ShadowMapSize = 1024;
					if (edit)
						edit(snapshot);
				};
				const auto image = Test::RenderGoldenScene(m_Gpu, m_Name, options);
				REQUIRE_MESSAGE(image.has_value(), image.error().ToString());
				REQUIRE(image->IsValid());
				CHECK(image->Width == RendererIIGoldenWidth);
				CHECK(image->Height == RendererIIGoldenHeight);
				return *image;
			}

			void Golden(const Image& image, const Test::GoldenSceneOptions& options = {})
			{
				// A fresh project copy and extraction must produce the same pixels without temporal history.
				CHECK(Capture(options).Pixels == image.Pixels);
				auto settings = Test::GetRepositoryGoldenSettings();
				if (settings.UpdateGolden)
					settings.GoldenRoot = Test::GetGoldenOutputDirectory() / "M9Candidates";
				const auto checked = Test::CheckGoldenImage(m_Name, image, m_Gpu.GetDevice().GetInfo().DeviceClass, {}, settings);
				const bool passed = checked.Outcome == Test::GoldenOutcome::Matched || checked.Outcome == Test::GoldenOutcome::Updated || checked.Outcome == Test::GoldenOutcome::SmokePassed;
				CHECK_MESSAGE(passed, checked.Message);
				if (checked.Outcome != Test::GoldenOutcome::Matched)
					MESSAGE(checked.Message);
			}

			void ReviewControl(std::string_view variation, const Image& image)
			{
				if (!Test::GetTestOptions().UpdateGolden)
					return;
				const auto directory = Test::GetGoldenOutputDirectory() / "M9Candidates" / "Controls";
				REQUIRE(FileSystem::CreateDirectories(directory));
				REQUIRE(WritePng(directory / Test::PathFromUtf8(std::format("{}-{}.png", m_Name, variation)), image));
			}
		private:
			Test::HeadlessGpuFixture& m_Gpu; // The calling test's GPU fixture outlives this adapter.
			std::string m_Name{};
		};

		[[nodiscard]] int RendererIIChannel(const Image& image, size_t pixel, size_t channel = 0)
		{
			return std::to_integer<int>(image.Pixels[pixel * 4 + channel]);
		}

		[[nodiscard]] size_t RendererIIChangedPixels(const Image& left, const Image& right, int threshold = 3)
		{
			REQUIRE(left.Pixels.size() == right.Pixels.size());
			size_t changed = 0;
			for (size_t pixel = 0; pixel < left.Pixels.size() / 4; ++pixel)
				if (std::max({ std::abs(RendererIIChannel(left, pixel) - RendererIIChannel(right, pixel)),
						std::abs(RendererIIChannel(left, pixel, 1) - RendererIIChannel(right, pixel, 1)),
						std::abs(RendererIIChannel(left, pixel, 2) - RendererIIChannel(right, pixel, 2)) })
					> threshold)
					++changed;
			return changed;
		}

		void RendererIICheckDarkening(const Image& dark, const Image& light, size_t minimumPixels)
		{
			REQUIRE(dark.Pixels.size() == light.Pixels.size());
			size_t darker = 0;
			int largest = 0;
			for (size_t pixel = 0; pixel < dark.Pixels.size() / 4; ++pixel)
			{
				const int delta = RendererIIChannel(light, pixel) - RendererIIChannel(dark, pixel);
				largest = std::max(largest, delta);
				if (delta > 4)
					++darker;
			}
			CAPTURE(darker);
			CAPTURE(largest);
			CHECK(darker > minimumPixels);
			CHECK(largest > 12);
		}

		void RendererIIShadowGolden(std::string_view name, bool far, bool orthographic, bool spots)
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			RendererIIGoldenFixture fixture(gpu, name);
			const Test::GoldenSceneOptions options{ .InspectSnapshot = [far, orthographic, spots](const RenderSnapshot& snapshot)
			{
				REQUIRE(snapshot.Lights.size() == (spots ? 2 : 1));
				CHECK((snapshot.Camera.ProjectionKind == RenderProjection::Orthographic) == orthographic);
				if (far)
				{
					const auto cascades = BuildShadowCascades(snapshot.Camera, snapshot.Lights[0], 0, 1024);
					REQUIRE(cascades);
					std::set<uint32_t> occupied;
					for (size_t mesh = 1; mesh < snapshot.Meshes.size(); ++mesh)
					{
						const float depth = -(snapshot.Camera.View * snapshot.Meshes[mesh].World[3]).z;
						for (uint32_t index = 0; index < cascades->Count; ++index)
							if (depth >= cascades->Cascades[index].SplitNear && depth <= cascades->Cascades[index].SplitFar)
								occupied.insert(index);
					}
					CHECK(occupied.size() >= 2);
				}
			} };
			const auto shadowed = fixture.Capture(options);
			const auto unshadowed = fixture.Capture({ .EditSnapshot = [](RenderSnapshot& snapshot)
			{
				for (auto& light : snapshot.Lights)
					light.CastShadows = false;
			} });
			RendererIICheckDarkening(shadowed, unshadowed, 250);
			fixture.ReviewControl("shadows-disabled", unshadowed);
			CHECK(fixture.Capture({ .EditSnapshot = [](RenderSnapshot& snapshot)
			{
				for (auto& mesh : snapshot.Meshes)
					mesh.ReceiveShadows = false;
			} }).Pixels
				== unshadowed.Pixels);
			CHECK(fixture.Capture({ .EditSnapshot = [](RenderSnapshot& snapshot)
			{
				for (auto& mesh : snapshot.Meshes)
					mesh.CastShadows = false;
			} }).Pixels
				== unshadowed.Pixels);
			fixture.Golden(shadowed, options);
		}

		void RendererIIAoGolden(std::string_view name, bool enabled, bool orthographic)
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			RendererIIGoldenFixture fixture(gpu, name);
			const auto actual = fixture.Capture({ .InspectSnapshot = [enabled, orthographic](const RenderSnapshot& snapshot)
			{
				CHECK(snapshot.Lights.empty());
				CHECK(snapshot.Post.SsaoEnabled == enabled);
				CHECK((snapshot.Camera.ProjectionKind == RenderProjection::Orthographic) == orthographic);
			} });
			const auto opposite = fixture.Capture({ .EditSnapshot = [enabled](RenderSnapshot& snapshot)
			{
				snapshot.Post.SsaoEnabled = !enabled;
			} });
			fixture.ReviewControl(enabled ? "ao-disabled" : "ao-enabled", opposite);
			RendererIICheckDarkening(enabled ? actual : opposite, enabled ? opposite : actual, 250);
			const auto ao = fixture.Capture({ .EditSnapshot = [](RenderSnapshot& snapshot)
			{
				snapshot.DebugView = RenderDebugView::AO;
			} });
			size_t occluded = 0;
			for (size_t pixel = 0; pixel < ao.Pixels.size() / 4; ++pixel)
			{
				REQUIRE(RendererIIChannel(ao, pixel) == RendererIIChannel(ao, pixel, 1));
				REQUIRE(RendererIIChannel(ao, pixel) == RendererIIChannel(ao, pixel, 2));
				if (RendererIIChannel(ao, pixel) < 245)
					++occluded;
				if (!enabled)
					REQUIRE(RendererIIChannel(ao, pixel) == 255);
			}
			CHECK((occluded > 0) == enabled);
			fixture.Golden(actual);
		}

		void RendererIICheckLabels(const RenderSnapshot& snapshot)
		{
			constexpr std::array<UUID, 2> Entities{ RendererIIAnnotatedCube, RendererIIAnnotatedSphere };
			constexpr std::array<std::string_view, 2> Names{ "Cube", "Sphere" };
			size_t labels = 0;
			for (const auto& command : snapshot.DebugDraw.GetCommands())
				if (const auto* text = std::get_if<DebugText>(&command.Shape))
				{
					REQUIRE(labels < Entities.size());
					CHECK(text->Text == std::format("{} {}", Names[labels], Entities[labels].ToString().substr(0, 6)));
					++labels;
				}
			REQUIRE(labels == 2);
		}

	}

	TEST_SUITE("Support")
	{
		TEST_CASE("RendererIIGolden: the common renderer rejects asset placeholders" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::ExpectLog missing(LogLevel::Error, "ASSET_MISSING");
			const auto image = Test::RenderGoldenScene(gpu, "ShadowsNear", { .EditSnapshot = [](RenderSnapshot& snapshot)
			{
				REQUIRE_FALSE(snapshot.Meshes.empty());
				snapshot.Meshes.front().Materials = { AssetHandle{ 0xfedcba9876543210 } };
			} });
			REQUIRE_FALSE(image);
			CHECK(image.error().GetCode() == ErrorCode::Validation);
			CHECK(image.error().ToString().contains("ASSET_MISSING"));
		}

		TEST_CASE("RendererIIGolden: scaffold reproduces the pinned scenes materials and metadata")
		{
			Test::AutomationFixture fixture("RendererIIScaffold", false);
			const auto project = Test::GetRepositoryRoot() / "Projects/FeatureTest";
			auto requests = BatchRunner::LoadFile(project / "Scaffold/Golden.jsonl");
			REQUIRE_MESSAGE(requests.has_value(), requests.error().ToString());
			// Replay the appended M9 section with EditorTestIdState; preceding M8 commands remain independent and unchanged.
			const auto first = std::find_if(requests->begin(), requests->end(), [](const BatchRequest& request)
			{
				if (request.Method != "asset.create")
					return false;
				const auto path = JsonReader(request.Params.Get()).ReadMember<std::string>("path");
				return path && *path == "Assets/Materials/Golden/RendererII/Grey.material";
			});
			REQUIRE(first != requests->end());
			requests->erase(requests->begin(), first);
			REQUIRE(requests->size() == 30);
			BatchRunner runner(std::move(*requests), {});
			auto& server = fixture.GetClient().GetServer();
			REQUIRE(Test::WaitUntil([&runner, &server]()
			{
				server.Pump();
				runner.Advance(server);
				return runner.IsFinished();
			}));
			REQUIRE_MESSAGE(!runner.GetFailure(), runner.GetFailedResponse().dump());
			CHECK(runner.GetResults().size() == 30);
			const auto refresh = fixture.GetEditor().GetAssets().Refresh();
			REQUIRE(refresh);
			CHECK(refresh->Diagnostics.empty());
			CHECK(refresh->CreatedMetas.empty());
			CHECK(fixture.GetEditor().GetAssets().GetDiagnostics().empty());
			const auto files = FileSystem::ListDirectory(fixture.GetEditorFixture().GetProjectRoot() / "Assets", true);
			REQUIRE(files);
			std::vector<std::filesystem::path> generated;
			for (const auto& file : *files)
			{
				const auto info = FileSystem::GetInfo(file);
				REQUIRE(info);
				if (info->IsDirectory || file.extension() == ".bak")
					continue;
				generated.push_back(file);
				if (Test::GetTestOptions().UpdateGolden)
				{
					const auto relative = file.lexically_relative(fixture.GetEditorFixture().GetProjectRoot());
					const auto destination = Test::GetGoldenOutputDirectory() / "M9Scaffold" / relative;
					const auto bytes = FileSystem::ReadFile(file);
					REQUIRE(bytes);
					REQUIRE(FileSystem::CreateDirectories(destination.parent_path()));
					REQUIRE(FileSystem::WriteFileAtomic(destination, *bytes, { .KeepBackup = false }));
				}
			}
			REQUIRE(generated.size() == 24);
			for (const auto& file : generated)
			{
				const auto relative = file.lexically_relative(fixture.GetEditorFixture().GetProjectRoot());
				CAPTURE(relative.generic_string());
				const auto actual = FileSystem::ReadFile(file);
				const auto expected = FileSystem::ReadFile(project / relative);
				REQUIRE(actual);
				REQUIRE(expected);
				CHECK(*actual == *expected);
			}
		}
	}

	TEST_SUITE(Test::GoldenSuite)
	{
		TEST_CASE("Golden: ShadowsNear")
		{
			RendererIIShadowGolden("ShadowsNear", false, false, false);
		}
		TEST_CASE("Golden: ShadowsFar")
		{
			RendererIIShadowGolden("ShadowsFar", true, false, false);
		}
		TEST_CASE("Golden: ShadowsOrtho")
		{
			RendererIIShadowGolden("ShadowsOrtho", false, true, false);
		}
		TEST_CASE("Golden: SpotShadows")
		{
			RendererIIShadowGolden("SpotShadows", false, false, true);
		}
		TEST_CASE("Golden: GtaoOn")
		{
			RendererIIAoGolden("GtaoOn", true, false);
		}
		TEST_CASE("Golden: GtaoOff")
		{
			RendererIIAoGolden("GtaoOff", false, false);
		}
		TEST_CASE("Golden: GtaoOrtho")
		{
			RendererIIAoGolden("GtaoOrtho", true, true);
		}

		TEST_CASE("Golden: SelectionOutline")
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			RendererIIGoldenFixture fixture(gpu, "SelectionOutline");
			const Test::GoldenSceneOptions options{ .EditSnapshot = [](RenderSnapshot& snapshot)
			{
				snapshot.Flags = RenderViewFlags::EditorOverlays | RenderViewFlags::Selection;
				snapshot.SelectedEntities = { RendererIIOutlineCube, RendererIIOutlineSphere };
			} };
			const auto outlined = fixture.Capture(options);
			const auto ordinary = fixture.Capture();
			fixture.ReviewControl("selection-disabled", ordinary);
			size_t visible = 0;
			size_t occluded = 0;
			const std::array<int, 3> orange = { 255, 196, 89 };
			for (size_t pixel = 0; pixel < outlined.Pixels.size() / 4; ++pixel)
			{
				bool full = true;
				bool dim = true;
				for (size_t channel = 0; channel < 3; ++channel)
				{
					full = full && std::abs(RendererIIChannel(outlined, pixel, channel) - orange[channel]) <= 1;
					dim = dim && std::abs(2 * RendererIIChannel(outlined, pixel, channel) - RendererIIChannel(ordinary, pixel, channel) - orange[channel]) <= 2;
				}
				visible += full ? 1 : 0;
				occluded += dim ? 1 : 0;
			}
			CAPTURE(visible);
			CAPTURE(occluded);
			CHECK(visible > 100);
			CHECK(occluded > 20);
			CHECK(RendererIIChangedPixels(outlined, ordinary) > 300);
			CHECK(fixture.Capture({ .EditSnapshot = [](RenderSnapshot& snapshot)
			{
				snapshot.Flags = RenderViewFlags::Selection;
				snapshot.SelectedEntities = { RendererIIOutlineCube, RendererIIOutlineSphere };
			} }).Pixels
				== ordinary.Pixels);
			fixture.Golden(outlined, options);
		}

		TEST_CASE("Golden: AnnotatedScreenshot")
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			RendererIIGoldenFixture fixture(gpu, "AnnotatedScreenshot");
			const auto ordinary = fixture.Capture();
			fixture.ReviewControl("annotations-disabled", ordinary);
			const auto labels = fixture.Capture({ .EditSnapshot = [](RenderSnapshot& snapshot)
			{
				snapshot.Annotations = { .Labels = RenderAnnotationLabels::Explicit, .LabelEntities = { RendererIIAnnotatedCube, RendererIIAnnotatedSphere } };
			},
				.InspectSnapshot = RendererIICheckLabels });
			CHECK(RendererIIChangedPixels(labels, ordinary) > 20);
			const auto lines = fixture.Capture({ .EditSnapshot = [](RenderSnapshot& snapshot)
			{
				snapshot.Flags = RenderViewFlags::Colliders;
				snapshot.Annotations = { .Bounds = true, .Axes = true };
			} });
			CHECK(RendererIIChangedPixels(lines, ordinary) > 500);
			const Test::GoldenSceneOptions options{ .EditSnapshot = [](RenderSnapshot& snapshot)
			{
				snapshot.Flags = RenderViewFlags::EditorOverlays | RenderViewFlags::Grid | RenderViewFlags::Icons | RenderViewFlags::Colliders;
				snapshot.Annotations = { .Labels = RenderAnnotationLabels::Explicit, .LabelEntities = { RendererIIAnnotatedCube, RendererIIAnnotatedSphere }, .Bounds = true, .Axes = true };
			},
				.InspectSnapshot = [](const RenderSnapshot& snapshot)
			{
				REQUIRE_FALSE(snapshot.Icons.empty());
				RendererIICheckLabels(snapshot);
			} };
			const auto image = fixture.Capture(options);
			CHECK(RendererIIChangedPixels(image, ordinary) > 1000);
			fixture.Golden(image, options);
		}
	}

}
