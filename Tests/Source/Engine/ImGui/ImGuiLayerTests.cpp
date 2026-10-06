#include "TestsPCH.h"

#include "Engine/ImGui/ImGuiLayer.h"

#include "Engine/App/EngineContext.h"
#include "Engine/App/ExitCode.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Graphics/FramePacer.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/OffscreenTarget.h"
#include "Engine/Graphics/PipelineFactory.h"
#include "Engine/Platform/Process.h"
#include "Support/HeadlessGpuFixture.h"
#include "Support/TempDirectory.h"
#include "Support/TestOptions.h"
#include "Support/Utf8Path.h"

#include <imgui.h>

namespace Engine {

	// A headless context with a null-platform window and a device, as an application has.
	static Result<Scope<EngineContext>> CreateRenderingContext()
	{
		return EngineContext::Create({
			.WorkerCount = 0,
			.Window = WindowSpecification{ .Title = "ImGuiLayer", .Width = 320, .Height = 200 },
			.Graphics = GraphicsSpecification{
				.Validation = true,
				.SynchronizationValidation = true,
				.MaxApiVersion = Test::GetTestOptions().VulkanApi,
			},
		});
	}

	TEST_SUITE("ImGui")
	{
		TEST_CASE("ImGuiLayer: frames take the given delta, not the wall clock, and render into a target"
			* doctest::test_suite(Test::GpuSuite) * doctest::skip(true))
		{
			Result<Scope<EngineContext>> created = CreateRenderingContext();
			if (!created.has_value())
			{
				Test::ReportGpuUnavailable(created.error().ToString());
				return;
			}
			EngineContext& context = **created;
			GraphicsDevice& device = *context.GetGraphicsDevice();
			Result<Scope<ImGuiLayer>> layer = ImGuiLayer::Create(*context.GetWindow(), device, *context.GetPipelineFactory(), {});
			REQUIRE_MESSAGE(layer.has_value(), layer.error().ToString());
			CHECK((*layer)->GetDrawData() == nullptr);
			CHECK((ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_DockingEnable) != 0);
			CHECK((ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable) == 0);

			Result<OffscreenTarget> target = OffscreenTarget::Create(device, { .Width = 320, .Height = 200 });
			REQUIRE_MESSAGE(target.has_value(), target.error().ToString());
			FramePacer pacer(device, 2);
			Result<nvrhi::CommandListHandle> commandList = device.CreateCommandList();
			REQUIRE_MESSAGE(commandList.has_value(), commandList.error().ToString());
			for (int frame = 0; frame < 3; ++frame)
			{
				pacer.BeginFrame();
				(*layer)->BeginFrame(0.25, pacer.GetFrameSlot());
				CHECK(ImGui::GetIO().DeltaTime == doctest::Approx(0.25f));
				ImGui::ShowDemoWindow();
				(*layer)->EndFrame();
				REQUIRE((*layer)->GetDrawData() != nullptr);
				(*commandList)->open();
				target->Clear(**commandList);
				const Status rendered = (*layer)->Render(**commandList, *target->GetFramebuffer());
				CHECK_MESSAGE(rendered.has_value(), (rendered.has_value() ? std::string() : rendered.error().ToString()));
				(*commandList)->close();
				pacer.EndFrame(device.ExecuteCommandList(**commandList));
				device.RunGarbageCollection();
			}
			device.WaitForIdle();
			layer->reset();
			CHECK(device.GetDiagnostics().GetErrorCount() == 0);
		}

		TEST_CASE("ImGuiLayer: SetIniFilePath saves the current settings and loads the new file"
			* doctest::test_suite(Test::GpuSuite) * doctest::skip(true))
		{
			Result<Scope<EngineContext>> created = CreateRenderingContext();
			if (!created.has_value())
			{
				Test::ReportGpuUnavailable(created.error().ToString());
				return;
			}
			EngineContext& context = **created;
			Test::TempDirectory directory("ImGuiIni");
			const std::filesystem::path launcherIni = directory / "User" / "Editor" / "imgui.ini";
			const std::filesystem::path projectIni = directory / "Project" / "Library" / "Editor" / "imgui.ini";
			Result<Scope<ImGuiLayer>> layer = ImGuiLayer::Create(*context.GetWindow(), *context.GetGraphicsDevice(),
				*context.GetPipelineFactory(), { .IniFilePath = launcherIni });
			REQUIRE_MESSAGE(layer.has_value(), layer.error().ToString());
			CHECK((*layer)->GetIniFilePath() == launcherIni);

			(*layer)->BeginFrame(1.0 / 60.0, 0);
			ImGui::Begin("Persisted");
			ImGui::End();
			(*layer)->EndFrame();

			// Switching saves the launcher's settings, which name the window, and points ImGui at the project's file.
			REQUIRE((*layer)->SetIniFilePath(projectIni).has_value());
			CHECK((*layer)->GetIniFilePath() == projectIni);
			const Result<std::string> saved = FileSystem::ReadText(launcherIni);
			REQUIRE_MESSAGE(saved.has_value(), saved.error().ToString());
			CHECK(saved->contains("[Window][Persisted]"));
			context.GetGraphicsDevice()->WaitForIdle();
		}

		TEST_CASE("ImGuiLayer: a second layer while one exists is InvalidState" * doctest::test_suite(Test::GpuSuite) * doctest::skip(true))
		{
			Result<Scope<EngineContext>> created = CreateRenderingContext();
			if (!created.has_value())
			{
				Test::ReportGpuUnavailable(created.error().ToString());
				return;
			}
			EngineContext& context = **created;
			Result<Scope<ImGuiLayer>> first = ImGuiLayer::Create(*context.GetWindow(), *context.GetGraphicsDevice(), *context.GetPipelineFactory(), {});
			REQUIRE_MESSAGE(first.has_value(), first.error().ToString());
			const Result<Scope<ImGuiLayer>> second =
				ImGuiLayer::Create(*context.GetWindow(), *context.GetGraphicsDevice(), *context.GetPipelineFactory(), {});
			REQUIRE_FALSE(second.has_value());
			CHECK(second.error().GetCode() == ErrorCode::InvalidState);
		}

		TEST_CASE("ImGuiLayer: imgui.ini is written under user:// before a project opens"
			* doctest::test_suite(Test::GpuSuite) * doctest::skip(true))
		{
			// §8.11: while no project is open (M5 has no projects), the editor keeps imgui.ini in user://Editor/, which is
			// <user-data root>/<AppName>/Editor/imgui.ini on disk; ImGui writes it at shutdown at the latest.
			Test::TempDirectory userData("EditorImGuiIni");
			const Result<std::filesystem::path> editor = Test::GetBuiltExecutablePath("Editor");
			REQUIRE_MESSAGE(editor.has_value(), editor.error().ToString());
			std::vector<std::string> arguments = { "--headless", "--frames", "5", "--user-data-dir=" + Test::PathToUtf8(userData.GetPath()) };
			const std::vector<std::string> gpuArguments = Test::GetGpuApplicationArguments();
			arguments.insert(arguments.end(), gpuArguments.begin(), gpuArguments.end());
			const Result<ProcessResult> result = Process::Run({ .Executable = *editor, .Arguments = std::move(arguments) }, std::chrono::seconds(60));
			REQUIRE_MESSAGE(result.has_value(), result.error().ToString());
			INFO("editor stderr: ", result->StandardError);
			CHECK(result->ExitCode == ExitCode::Success);
			const Result<std::string> ini = FileSystem::ReadText(userData / ENGINE_PRODUCT_NAME / "Editor" / "imgui.ini");
			REQUIRE_MESSAGE(ini.has_value(), ini.error().ToString());
			CHECK(ini->contains("[Window][Dear ImGui Demo]"));
		}
	}

}
