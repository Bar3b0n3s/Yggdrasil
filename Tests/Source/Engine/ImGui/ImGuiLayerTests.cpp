#include "TestsPCH.h"

#include "Engine/ImGui/ImGuiLayer.h"

#include "Engine/App/EngineContext.h"
#include "Engine/App/ExitCode.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Graphics/FramePacer.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/OffscreenTarget.h"
#include "Engine/Graphics/PipelineFactory.h"
#include "Engine/Platform/GlfwLibrary.h"
#include "Engine/Platform/Process.h"
#include "Engine/Platform/Window.h"
#include "Support/HeadlessGpuFixture.h"
#include "Support/TempDirectory.h"
#include "Support/TestOptions.h"
#include "Support/Utf8Path.h"
#include "Support/WindowedChild.h"

#include <imgui.h>

#include <string_view>

namespace Engine {

	// The windowed child that runs the vendored GLFW backend on a native window.
	static constexpr std::string_view NativeBackendTarget = "ImGuiLayer: the GLFW backend drives the UI of a native window";

	// A context with a window and a device, as an application has: a null-platform window in the (headless) Tests process,
	// a native one in a windowed child.
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
			* doctest::test_suite(Test::GpuSuite))
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
			CHECK(ImGui::GetIO().IniFilename == nullptr);
			// The Tests process is headless: the layer is the platform backend of GLFW's null platform.
			CHECK(ImGui::GetIO().BackendPlatformName != nullptr);
			CHECK(ImGui::GetIO().BackendRendererName != nullptr);

			Result<OffscreenTarget> target = OffscreenTarget::Create(device, { .Width = 320, .Height = 200 });
			REQUIRE_MESSAGE(target.has_value(), target.error().ToString());
			FramePacer pacer(device, 2);
			Result<nvrhi::CommandListHandle> commandList = device.CreateCommandList();
			REQUIRE_MESSAGE(commandList.has_value(), commandList.error().ToString());
			// Nothing to record before the first UI frame ends.
			const Status early = (*layer)->Render(**commandList, *target->GetFramebuffer());
			REQUIRE_FALSE(early.has_value());
			CHECK(early.error().GetCode() == ErrorCode::InvalidState);
			for (int frame = 0; frame < 3; ++frame)
			{
				pacer.BeginFrame();
				// A SystemClock's first delta is 0; Dear ImGui needs a positive one, so it is clamped to the minimum.
				const double delta = frame == 0 ? 0.0 : 0.25;
				(*layer)->BeginFrame(delta, pacer.GetFrameSlot());
				if (frame == 0)
					CHECK(ImGui::GetIO().DeltaTime == static_cast<float>(ImGuiLayer::MinimumDeltaSeconds));
				else
					CHECK(ImGui::GetIO().DeltaTime == doctest::Approx(0.25f));
				CHECK(ImGui::GetIO().DisplaySize.x == 320.0f);
				CHECK(ImGui::GetIO().DisplaySize.y == 200.0f);
				CHECK((*layer)->GetDrawData() == nullptr); // a UI frame is being built
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

			// An editor screenshot re-renders the last UI frame into another target of the same size, in a submission of its
			// own, while the frame's submission may still be running (§8.13; synchronization validation checks the reuse of
			// the frame slot's buffers).
			Result<OffscreenTarget> screenshotTarget = OffscreenTarget::Create(device, { .Width = 320, .Height = 200 });
			REQUIRE_MESSAGE(screenshotTarget.has_value(), screenshotTarget.error().ToString());
			REQUIRE((*layer)->GetDrawData() != nullptr);
			(*commandList)->open();
			screenshotTarget->Clear(**commandList);
			const Status rerendered = (*layer)->Render(**commandList, *screenshotTarget->GetFramebuffer());
			CHECK_MESSAGE(rerendered.has_value(), (rerendered.has_value() ? std::string() : rerendered.error().ToString()));
			(*commandList)->close();
			device.ExecuteCommandList(**commandList);

			device.WaitForIdle();
			layer->reset();
			CHECK(device.GetDiagnostics().GetErrorCount() == 0);
		}

		TEST_CASE("ImGuiLayer: SetIniFilePath saves the current settings and loads the new file"
			* doctest::test_suite(Test::GpuSuite))
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
			// The project's layout puts the window somewhere else.
			REQUIRE(FileSystem::CreateDirectories(projectIni.parent_path()).has_value());
			const std::string projectLayout = "[Window][Persisted]\nPos=123,45\nSize=150,90\nCollapsed=0\n";
			REQUIRE(FileSystem::WriteFileAtomic(projectIni, std::as_bytes(std::span(projectLayout))).has_value());

			Result<Scope<ImGuiLayer>> layer = ImGuiLayer::Create(*context.GetWindow(), *context.GetGraphicsDevice(),
				*context.GetPipelineFactory(), { .IniFilePath = launcherIni });
			REQUIRE_MESSAGE(layer.has_value(), layer.error().ToString());
			CHECK((*layer)->GetIniFilePath() == launcherIni);
			// Create made the folder, so ImGui's own writes can succeed.
			CHECK(FileSystem::Exists(launcherIni.parent_path()));

			(*layer)->BeginFrame(1.0 / 60.0, 0);
			ImGui::SetNextWindowPos(ImVec2(10.0f, 20.0f), ImGuiCond_FirstUseEver);
			ImGui::Begin("Persisted");
			ImGui::End();
			(*layer)->EndFrame();

			// Switching saves the launcher's settings, which name the window, and points ImGui at the project's file.
			REQUIRE((*layer)->SetIniFilePath(projectIni).has_value());
			CHECK((*layer)->GetIniFilePath() == projectIni);
			const Result<std::string> saved = FileSystem::ReadText(launcherIni);
			REQUIRE_MESSAGE(saved.has_value(), saved.error().ToString());
			CHECK(saved->contains("[Window][Persisted]"));
			CHECK(saved->contains("Pos=10,20"));

			// The project's file was loaded: the live window moves to the position it stores.
			(*layer)->BeginFrame(1.0 / 60.0, 0);
			ImGui::Begin("Persisted");
			const ImVec2 position = ImGui::GetWindowPos();
			ImGui::End();
			(*layer)->EndFrame();
			CHECK(position.x == 123.0f);
			CHECK(position.y == 45.0f);
			context.GetGraphicsDevice()->WaitForIdle();
			layer->reset();

			// Destroying the layer saves the settings to the current file only.
			const Result<std::string> projectSaved = FileSystem::ReadText(projectIni);
			REQUIRE_MESSAGE(projectSaved.has_value(), projectSaved.error().ToString());
			CHECK(projectSaved->contains("Pos=123,45"));
			const Result<std::string> launcherAfter = FileSystem::ReadText(launcherIni);
			REQUIRE_MESSAGE(launcherAfter.has_value(), launcherAfter.error().ToString());
			CHECK(*launcherAfter == *saved);
		}

		TEST_CASE("ImGuiLayer: a second layer while one exists is InvalidState" * doctest::test_suite(Test::GpuSuite))
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
			* doctest::test_suite(Test::GpuSuite))
		{
			// §8.11: while no project is open (M5 has no projects), the editor keeps imgui.ini in user://Editor/, which is
			// <user-data root>/<AppName>/Editor/imgui.ini on disk; ImGui writes it at shutdown at the latest. The editor
			// renders, so the test needs a device like every GPU test; the probe device is gone before the editor starts.
			if (!Test::ProbeGpuForProcess())
				return;
			Test::TempDirectory userData("EditorImGuiIni");
			const Result<std::filesystem::path> editor = Test::GetBuiltExecutablePath("Editor");
			REQUIRE_MESSAGE(editor.has_value(), editor.error().ToString());
			std::vector<std::string> arguments = { "--headless", "--frames", "5", "--user-data-dir=" + Test::PathToUtf8(userData.GetPath()) };
			const std::vector<std::string> gpuArguments = Test::GetGpuApplicationArguments();
			arguments.insert(arguments.end(), gpuArguments.begin(), gpuArguments.end());
			const Result<ProcessResult> result =
				Process::Run({ .Executable = *editor, .Arguments = std::move(arguments) }, std::chrono::seconds(60));
			REQUIRE_MESSAGE(result.has_value(), result.error().ToString());
			INFO("editor stderr: ", result->StandardError);
			CHECK(result->ExitCode == ExitCode::Success);
			CHECK(Test::FindProblemLogLines(result->StandardError).empty());
			const Result<std::string> ini = FileSystem::ReadText(userData / ENGINE_PRODUCT_NAME / "Editor" / "imgui.ini");
			REQUIRE_MESSAGE(ini.has_value(), ini.error().ToString());
			CHECK(ini->contains("[Window][Dear ImGui Demo]"));
		}

		TEST_CASE("ImGuiLayer: on a native platform the vendored GLFW backend is the platform side" * doctest::test_suite(Test::GpuSuite))
		{
			// The Tests process is headless, so the native platform runs in a windowed child (Support/WindowedChild.h), with
			// the GPU options of this run.
			if (!Test::ProbeGpuForProcess())
				return;
			std::vector<std::string> arguments = { std::format("--windowed-child={}", NativeBackendTarget) };
			const std::vector<std::string> gpuArguments = Test::GetGpuTestsChildArguments();
			arguments.insert(arguments.end(), gpuArguments.begin(), gpuArguments.end());
			const Result<ProcessResult> child = Process::Run(Test::MakeTestsChildSpecification(std::move(arguments)), Test::DefaultWindowedChildTimeout);
			REQUIRE_MESSAGE(child.has_value(), child.error().ToString());
			INFO("child output: ", child->StandardOutput);
			INFO("child log: ", child->StandardError);
			CHECK(child->ExitCode == 0);
		}

		// The target of the test above (NativeBackendTarget); it runs only in the windowed child.
		TEST_CASE("ImGuiLayer: the GLFW backend drives the UI of a native window"
			* doctest::test_suite(Test::ChildTargetSuite) * doctest::skip(true))
		{
			REQUIRE(GlfwLibrary::GetPlatform() == Test::GetNativeGlfwPlatform());
			Result<Scope<EngineContext>> created = CreateRenderingContext();
			if (!created.has_value())
			{
				Test::ReportGpuUnavailable(created.error().ToString());
				return;
			}
			EngineContext& context = **created;
			GraphicsDevice& device = *context.GetGraphicsDevice();
			Window& window = *context.GetWindow();
			Result<OffscreenTarget> target =
				OffscreenTarget::Create(device, { .Width = window.GetFramebufferWidth(), .Height = window.GetFramebufferHeight() });
			REQUIRE_MESSAGE(target.has_value(), target.error().ToString());
			FramePacer pacer(device, 2);
			Result<nvrhi::CommandListHandle> commandList = device.CreateCommandList();
			REQUIRE_MESSAGE(commandList.has_value(), commandList.error().ToString());

			// Two layers one after the other: the backend's shutdown restores the window's own callbacks, so the second layer
			// chains to them rather than to the first layer's.
			for (int layerIndex = 0; layerIndex < 2; ++layerIndex)
			{
				Result<Scope<ImGuiLayer>> layer = ImGuiLayer::Create(window, device, *context.GetPipelineFactory(), {});
				REQUIRE_MESSAGE(layer.has_value(), layer.error().ToString());
				REQUIRE(ImGui::GetIO().BackendPlatformName != nullptr);
				CHECK(std::string_view(ImGui::GetIO().BackendPlatformName).starts_with("imgui_impl_glfw"));
				for (int frame = 0; frame < 3; ++frame)
				{
					// OS events pass through the backend's callbacks, which chain to the window's own.
					window.PollEvents();
					pacer.BeginFrame();
					(*layer)->BeginFrame(1.0 / 60.0, pacer.GetFrameSlot());
					// The backend reads the window's size every frame.
					CHECK(ImGui::GetIO().DisplaySize.x == static_cast<float>(window.GetWidth()));
					CHECK(ImGui::GetIO().DisplaySize.y == static_cast<float>(window.GetHeight()));
					ImGui::ShowDemoWindow();
					(*layer)->EndFrame();
					(*commandList)->open();
					target->Clear(**commandList);
					const Status rendered = (*layer)->Render(**commandList, *target->GetFramebuffer());
					CHECK_MESSAGE(rendered.has_value(), (rendered.has_value() ? std::string() : rendered.error().ToString()));
					(*commandList)->close();
					pacer.EndFrame(device.ExecuteCommandList(**commandList));
					device.RunGarbageCollection();
				}
				device.WaitForIdle();
			}
			CHECK(device.GetDiagnostics().GetErrorCount() == 0);
		}
	}

}
