#include "TestsPCH.h"

#include "Editor/Private/EditorHostAutosave.h"
#include "EditorCore/Autosave/Autosave.h"
#include "EditorCore/Commands/SceneEdit.h"
#include "EditorCore/EditorContext.h"
#include "Engine/App/EngineContext.h"
#include "Engine/App/ExitCode.h"
#include "Engine/App/ProcessContext.h"
#include "Engine/Core/Assert.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Log.h"
#include "Engine/Graphics/FramePacer.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Platform/Process.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/SceneSerializer.h"
#include "Support/ChildOutput.h"
#include "Support/DeathTest.h"
#include "Support/EditorTestFixture.h"
#include "Support/HeadlessGpuFixture.h"
#include "Support/TempDirectory.h"
#include "Support/TestOptions.h"
#include "Support/Utf8Path.h"

#include <span>
#include <string>

// The editor rendering through the engine's frame, and the GPU fault paths of Architecture §8.14 item 8, as whole
// processes (Roadmap M5 "Fault injection"). Every run is headless, so no error dialog can block it, and gets
// --user-data-dir, so logs and crash reports land in the test's temporary directory.

namespace Engine {

	// Uses the exact host callback with a real submission/wait fault, after publishing a dirty scene. Unlike startup
	// injection, this reaches a populated snapshot. Later live edits must never be read by the failing-thread hook.
	static Status RunAutosaveGpuFaultChild(GpuFault fault)
	{
		ProcessContext* process = ProcessContext::GetCurrent();
		if (!process)
			return MakeError(ErrorCode::InvalidState, "Missing process context");
		ENGINE_TRY_ASSIGN(auto engine, EngineContext::Create({ .UserDataDirectory = process->GetUserDataPaths().Root }));
		ENGINE_TRY_ASSIGN(auto editor, EditorContext::Create(*engine, { .IdGeneratorState = Test::EditorTestIdState }));
		ENGINE_TRY_ASSIGN(auto project, ProjectManager::OpenProject(FileSystem::PathFromUtf8(Test::GetTestOptions().ChildArgument), {}, editor->GetTypeRegistry()));
		ENGINE_TRY(editor->OpenProject(std::move(project)));
		ENGINE_TRY_ASSIGN(const auto path, VfsPath::Create("project", "Assets/Scenes/Main.scene"));
		auto scene = editor->CreateScene("Main");
		LoadReport report;
		ENGINE_TRY(SceneSerializer::LoadFromFile(*scene, editor->GetVfs(), path, {}, report));
		editor->SetScene(std::move(scene), path);
		Autosave saves(*editor);
		{
			SceneEdit edit(*editor, "Published before fault");
			(void)editor->GetScene().CreateEntity("PublishedBeforeFault");
			ENGINE_TRY(edit.Commit());
		}
		ENGINE_TRY(saves.Publish());
		{
			SceneEdit edit(*editor, "Not published");
			(void)editor->GetScene().CreateEntity("NotPublishedByHost");
			ENGINE_TRY(edit.Commit());
		}
		// Establish the device and command list before installing the borrowed hook so setup errors can return normally.
		Test::HeadlessGpuFixture gpu({ .InjectFault = fault });
		if (!gpu.IsAvailable())
			return MakeError(ErrorCode::Gpu, "{}", gpu.GetUnavailableReason());
		GraphicsDevice& device = gpu.GetDevice();
		ENGINE_TRY_ASSIGN(auto commandList, device.CreateCommandList());
		commandList->open();
		commandList->close();
		// An injected device loss strikes the first submission after a garbage collection, as in an application's second
		// frame (Docs/Decisions/0029-m18-contract.md decision 30).
		device.RunGarbageCollection();
		process->SetFatalErrorHook(MakeEditorAutosaveFatalHook(saves));
		const uint64_t submission = device.ExecuteCommandList(*commandList);
		// A hang spends the wait's budget and then its reset wait without a loss report, which stays FatalError(GpuHang)
		// (decision 40).
		WaitForSubmission(device, submission, "Editor autosave fault probe");
		// The loss is reported and returns; its recovery fails after the one retry, which ends the process as an
		// application's frame boundary does (decision 29).
		if (device.IsLost())
		{
			commandList = nullptr;
			const Status recovered = device.Recover(nullptr);
			if (!recovered.has_value())
				device.RaiseDeviceLost(std::format("Editor autosave fault probe: {}", recovered.error().ToString()));
		}
		// Only a broken fault injection reaches here; retain lifetime discipline even on that failed-test path.
		process->SetFatalErrorHook({});
		ENGINE_VERIFY(saves.Reset(), "fatal callback quiesced");
		return MakeError(ErrorCode::InvalidState, "Injected GPU fault did not terminate the child");
	}

	// The unrecoverable loss: from M18 a plain device loss is recovered (Docs/Decisions/0029-m18-contract.md decision 30).
	ENGINE_DEATH_TEST("Editor/AutosaveDeviceLost")
	{
		const auto result = RunAutosaveGpuFaultChild(GpuFault::DeviceLostUnrecoverable);
		if (!result)
			ENGINE_ERROR("Autosave fault child: {}", result.error());
	}

	ENGINE_DEATH_TEST("Editor/AutosaveGpuHang")
	{
		const auto result = RunAutosaveGpuFaultChild(GpuFault::Hang);
		if (!result)
			ENGINE_ERROR("Autosave fault child: {}", result.error());
	}

	static Result<ProcessResult> RunRenderingEditor(const Test::TempDirectory& userData, std::vector<std::string> arguments,
		std::chrono::milliseconds timeout = std::chrono::seconds(60))
	{
		ENGINE_TRY_ASSIGN(std::filesystem::path editor, Test::GetBuiltExecutablePath("Editor"));
		const std::vector<std::string> gpuArguments = Test::GetGpuApplicationArguments();
		arguments.insert(arguments.end(), gpuArguments.begin(), gpuArguments.end());
		arguments.push_back("--user-data-dir=" + Test::PathToUtf8(userData.GetPath()));
		return Process::Run({ .Executable = std::move(editor), .Arguments = std::move(arguments) }, timeout);
	}

	TEST_SUITE("Editor")
	{
		TEST_CASE("EditorApp: its fatal hook preserves a published dirty scene through device loss and hang" * doctest::test_suite(Test::GpuSuite))
		{
			if (!Test::ProbeGpuForProcess())
				return;
			std::string death;
			std::string reason;
			SUBCASE("device loss")
			{
				death = "Editor/AutosaveDeviceLost";
				reason = "DeviceLost";
			}
			SUBCASE("bounded wait hang")
			{
				death = "Editor/AutosaveGpuHang";
				reason = "GpuHang";
			}
			Test::EditorTestFixture fixture("EditorFatalAutosave");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			const auto projectFile = fixture.GetEditor().GetProject().GetProjectFile();
			const auto source = fixture.GetProjectRoot() / "Assets/Scenes/Main.scene";
			const auto original = FileSystem::ReadText(source);
			REQUIRE(original.has_value());
			REQUIRE(fixture.GetEditor().CloseProject().has_value());
			std::vector<std::string> arguments = Test::GetGpuTestsChildArguments();
			arguments.push_back("--death-test=" + death);
			arguments.push_back("--child-argument=" + Test::PathToUtf8(projectFile));
			arguments.push_back("--user-data-dir=" + Test::PathToUtf8(fixture.GetDirectory().GetPath()));
			const auto result = Process::Run(Test::MakeTestsChildSpecification(std::move(arguments)), std::chrono::seconds(60));
			REQUIRE(result.has_value());
			INFO(result->StandardError);
			REQUIRE(result->ExitCode == ExitCode::Crash);
			const auto crash = Test::ReadOnlyCrashReport(fixture.GetDirectory().GetPath());
			REQUIRE(crash.has_value());
			CHECK(crash->contains("Reason: Fatal error (" + reason + ")"));
			CHECK(Test::FindGpuMessageLines(result->StandardError).empty());
			CHECK(FileSystem::ReadText(source).value_or("") == *original);
			// Reopen through the normal validated recovery service, proving a complete manifest, identity and payload.
			auto project = ProjectManager::OpenProject(projectFile, {}, fixture.GetEditor().GetTypeRegistry());
			REQUIRE(project.has_value());
			REQUIRE(fixture.GetEditor().OpenProject(std::move(*project)).has_value());
			Autosave saves(fixture.GetEditor());
			const auto recovery = saves.FindRecovery();
			REQUIRE(recovery.has_value());
			REQUIRE(recovery->has_value());
			REQUIRE(saves.Recover(**recovery).has_value());
			const auto recovered = SceneSerializer::SaveToString(fixture.GetEditor().GetScene());
			REQUIRE(recovered.has_value());
			CHECK(recovered->contains("PublishedBeforeFault"));
			CHECK_FALSE(recovered->contains("NotPublishedByHost"));
			CHECK(fixture.GetEditor().IsSceneDirty());
			CHECK(FileSystem::ReadText(source).value_or("") == *original);
		}

		TEST_CASE("EditorApp: --headless --frames 10 renders with the Vulkan renderer and exits 0"
			* doctest::test_suite(Test::GpuSuite))
		{
			if (!Test::ProbeGpuForProcess())
				return;
			Test::TempDirectory userData("EditorRenders");
			const Result<ProcessResult> result = RunRenderingEditor(userData, { "--headless", "--frames", "10" });
			REQUIRE_MESSAGE(result.has_value(), result.error().ToString());
			INFO("editor stderr: ", result->StandardError);
			CHECK(result->ExitCode == ExitCode::Success);
			CHECK(result->StandardError.contains("Process context: VulkanLoader initialized"));
			// Validation is on (GetGpuApplicationArguments): nothing it reports, and no other error.
			CHECK(Test::FindProblemLogLines(result->StandardError).empty());
		}

		// The command-line faults strike at the first submissions, before any request could open or edit a scene, so these
		// two runs prove the exit code and the crash report only. The editor's autosave on those faults is proven by
		// "EditorApp: its fatal hook preserves a published dirty scene through device loss and hang" above (the production
		// hook, MakeEditorAutosaveFatalHook, under both real faults) and, through the real EditorApp, by the Python
		// test_device_loss_autosaves_current_dirty_scene (debug.deviceLost {unrecoverable} after a dirty edit, then recover:
		// true). The fatal loss is the unrecoverable one: from M18 a plain device loss is recovered
		// (Docs/Decisions/0029-m18-contract.md decision 30; "EditorApp: --gpu-inject-fault=device-lost recovers the device
		// and runs to the end").
		TEST_CASE("EditorApp: --gpu-inject-fault=device-lost-unrecoverable exits 4 with a crash report"
			* doctest::test_suite(Test::GpuSuite))
		{
			if (!Test::ProbeGpuForProcess())
				return;
			Test::TempDirectory userData("EditorDeviceLost");
			const Result<ProcessResult> result =
				RunRenderingEditor(userData, { "--headless", "--frames", "10", "--gpu-inject-fault", "device-lost-unrecoverable" });
			REQUIRE_MESSAGE(result.has_value(), result.error().ToString());
			INFO("editor stderr: ", result->StandardError);
			CHECK(result->ExitCode == ExitCode::Crash);
			const Result<std::string> report = Test::ReadOnlyCrashReport(userData.GetPath());
			REQUIRE_MESSAGE(report.has_value(), report.error().ToString());
			CHECK(report->contains("Reason: Fatal error (DeviceLost)"));
			CHECK(Test::FindGpuMessageLines(result->StandardError).empty());
		}

		TEST_CASE("EditorApp: --gpu-inject-fault=device-lost recovers the device and runs to the end"
			* doctest::test_suite(Test::GpuSuite))
		{
			// M18 acceptance (C18.3, Docs/Decisions/0029-m18-contract.md decisions 29 and 30): the loss strikes the second
			// frame, the frame boundary recovers it, and the editor renders its remaining frames and exits 0, with no crash
			// report and no GPU message (--expect-no-gpu-errors, GetGpuApplicationArguments).
			if (!Test::ProbeGpuForProcess())
				return;
			Test::TempDirectory userData("EditorDeviceLostRecovered");
			const Result<ProcessResult> result =
				RunRenderingEditor(userData, { "--headless", "--frames", "10", "--gpu-inject-fault", "device-lost" });
			REQUIRE_MESSAGE(result.has_value(), result.error().ToString());
			INFO("editor stderr: ", result->StandardError);
			CHECK(result->ExitCode == ExitCode::Success);
			CHECK(result->StandardError.contains("GPU device recovered"));
			const std::vector<std::filesystem::path> reports = Test::ListCrashFiles(userData / ENGINE_PRODUCT_NAME / "Crashes", ".txt");
			CHECK(reports.empty());
			CHECK(Test::FindGpuMessageLines(result->StandardError).empty());
		}

		TEST_CASE("EditorApp: --gpu-inject-fault=hang-reset recovers the device and runs to the end"
			* doctest::test_suite(Test::GpuSuite))
		{
			// M18 acceptance (C18.4, Docs/Decisions/0029-m18-contract.md decisions 40 and 46): the first bounded wait after the
			// first frame spends its budget, its reset wait reports the loss, the frame boundary recovers it like any loss, and
			// the editor renders its remaining frames and exits 0, with no crash report and no GPU message.
			if (!Test::ProbeGpuForProcess())
				return;
			Test::TempDirectory userData("EditorHangReset");
			const Result<ProcessResult> result =
				RunRenderingEditor(userData, { "--headless", "--frames", "10", "--gpu-inject-fault", "hang-reset" });
			REQUIRE_MESSAGE(result.has_value(), result.error().ToString());
			INFO("editor stderr: ", result->StandardError);
			CHECK(result->ExitCode == ExitCode::Success);
			CHECK(result->StandardError.contains("GPU device recovered"));
			const std::vector<std::filesystem::path> reports = Test::ListCrashFiles(userData / ENGINE_PRODUCT_NAME / "Crashes", ".txt");
			CHECK(reports.empty());
			CHECK(Test::FindGpuMessageLines(result->StandardError).empty());
		}

		TEST_CASE("EditorApp: --gpu-inject-fault=oom-rendertarget-persistent fails only its requests and runs to the end"
			* doctest::test_suite(Test::GpuSuite))
		{
			// M18 acceptance (C18.4, decisions 42, 44 and 46): every render target created after the first frame runs out of
			// memory, the views' new sizes among them, so each such request fails alone after its trim and retry; the editor
			// keeps running to its last frame and exits 0, with no crash report and no GPU message. The editor opens a project:
			// without one its views render nothing, and every target it creates exists before the fault is armed (the views'
			// renderers and the frame target at startup), so nothing would strike. With one, the first frame renders the views
			// at their startup size (640 x 360) before its UI lays out their panels (OnRender runs before OnImGuiRender), and a
			// later frame resizes them to the panels' sizes, after the first garbage collection armed the fault.
			if (!Test::ProbeGpuForProcess())
				return;
			Test::EditorTestFixture fixture("EditorOomRenderTargetPersistent");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			const std::filesystem::path projectFile = fixture.GetEditor().GetProject().GetProjectFile();
			REQUIRE(fixture.GetEditor().CloseProject().has_value());
			const Result<ProcessResult> result = RunRenderingEditor(fixture.GetDirectory(),
				{ "--headless", "--project", Test::PathToUtf8(projectFile), "--frames", "10", "--gpu-inject-fault", "oom-rendertarget-persistent",
					"--engine-cache-dir=" + Test::PathToUtf8(fixture.GetDirectory() / "EngineCache") });
			REQUIRE_MESSAGE(result.has_value(), result.error().ToString());
			INFO("editor stderr: ", result->StandardError);
			CHECK(result->ExitCode == ExitCode::Success);
			CHECK(result->StandardError.contains("injected by --gpu-inject-fault=oom-rendertarget-persistent"));
			const std::vector<std::filesystem::path> reports =
				Test::ListCrashFiles(fixture.GetDirectory() / ENGINE_PRODUCT_NAME / "Crashes", ".txt");
			CHECK(reports.empty());
			CHECK(Test::FindGpuMessageLines(result->StandardError).empty());
		}

		TEST_CASE("EditorApp: a view render that runs out of memory logs one warning and the run ends without errors"
			* doctest::test_suite(Test::GpuSuite))
		{
			// M18 acceptance (C18.4, decisions 44 and 46): a view's per-frame render fails alone. After the first frame armed
			// oom-rendertarget-persistent, a batch selects a cube, so every later frame's scene view needs its selection targets
			// and runs out of memory: the editor logs one warning for the run of failing frames (never an error, which
			// --expect-no-errors would turn into exit code 1), the views trim after each such frame's submission
			// (EditorHostViewports, tested in process), and the batch ends with exit code 0, without a crash report.
			if (!Test::ProbeGpuForProcess())
				return;
			Test::EditorTestFixture fixture("EditorOomViewRender");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			const std::filesystem::path projectFile = fixture.GetEditor().GetProject().GetProjectFile();
			REQUIRE(fixture.GetEditor().CloseProject().has_value());
			std::string requests = std::string(R"({"method":"scene.open","params":{"path":"Assets/Scenes/Main.scene"}})") + "\n"
				+ R"({"method":"entity.create","params":{"name":"Cube","components":{"MeshRenderer":{"Mesh":"engine://Meshes/Cube"}}}})" + "\n"
				+ R"({"method":"edit.select","params":{"entities":["/Cube"]}})" + "\n";
			// Frames after the selection: one request each at least.
			for (int frame = 0; frame < 4; ++frame)
				requests += std::string(R"({"method":"edit.getSelection","params":{}})") + "\n";
			const std::filesystem::path batch = fixture.GetDirectory() / "Select.jsonl";
			REQUIRE(FileSystem::WriteFileAtomic(batch, std::as_bytes(std::span(requests.data(), requests.size()))).has_value());
			const Result<ProcessResult> result = RunRenderingEditor(fixture.GetDirectory(),
				{ "--headless", "--project", Test::PathToUtf8(projectFile), "--batch", Test::PathToUtf8(batch), "--expect-no-errors", "--gpu-inject-fault",
					"oom-rendertarget-persistent", "--engine-cache-dir=" + Test::PathToUtf8(fixture.GetDirectory() / "EngineCache") });
			REQUIRE_MESSAGE(result.has_value(), result.error().ToString());
			INFO("editor stderr: ", result->StandardError);
			CHECK(result->ExitCode == ExitCode::Success);
			size_t warnings = 0;
			for (size_t at = result->StandardError.find("Editor views ran out of GPU memory"); at != std::string::npos;
				at = result->StandardError.find("Editor views ran out of GPU memory", at + 1))
				++warnings;
			CHECK(warnings == 1);
			CHECK_FALSE(result->StandardError.contains("Cannot render editor views"));
			const std::vector<std::filesystem::path> reports =
				Test::ListCrashFiles(fixture.GetDirectory() / ENGINE_PRODUCT_NAME / "Crashes", ".txt");
			CHECK(reports.empty());
			CHECK(Test::FindGpuMessageLines(result->StandardError).empty());
		}

		TEST_CASE("EditorApp: out-of-memory at startup still exits 4 with a crash report"
			* doctest::test_suite(Test::GpuSuite))
		{
			// M18 acceptance (C18.4, decisions 42 and 46): "out-of-memory at startup still exits 4". ENGINE_GPU_FAULT_AT_STARTUP
			// arms oom-rendertarget-persistent at the device's creation, so the editor's startup targets run out of memory, which
			// stays FatalError(OutOfMemory) with the editor's fatal hook: nothing has rendered, and nothing is retried as a
			// request.
			if (!Test::ProbeGpuForProcess())
				return;
			Test::TempDirectory userData("EditorOomAtStartup");
			const Result<std::filesystem::path> editor = Test::GetBuiltExecutablePath("Editor");
			REQUIRE_MESSAGE(editor.has_value(), editor.error().ToString());
			std::vector<std::string> arguments = { "--headless", "--frames", "10", "--gpu-inject-fault", "oom-rendertarget-persistent" };
			const std::vector<std::string> gpuArguments = Test::GetGpuApplicationArguments();
			arguments.insert(arguments.end(), gpuArguments.begin(), gpuArguments.end());
			arguments.push_back("--user-data-dir=" + Test::PathToUtf8(userData.GetPath()));
			const Result<ProcessResult> result = Process::Run({ .Executable = *editor, .Arguments = std::move(arguments), .Environment = { { std::string(GpuFaultAtStartupVariable), "1" } } },
				std::chrono::seconds(60));
			REQUIRE_MESSAGE(result.has_value(), result.error().ToString());
			INFO("editor stderr: ", result->StandardError);
			CHECK(result->ExitCode == ExitCode::Crash);
			const Result<std::string> report = Test::ReadOnlyCrashReport(userData.GetPath());
			REQUIRE_MESSAGE(report.has_value(), report.error().ToString());
			CHECK(report->contains("Reason: Fatal error (OutOfMemory)"));
			CHECK(Test::FindGpuMessageLines(result->StandardError).empty());
		}

		TEST_CASE("EditorApp: --gpu-inject-fault=hang exits 4 with a crash report after the bounded wait"
			* doctest::test_suite(Test::GpuSuite))
		{
			// The first bounded wait, the frame pacer's at the first frame whose slot has a submission, skips its early-outs and
			// spends its whole budget of timed-out slices and then its reset wait's, since no loss report arrives (FramePacer.h;
			// Docs/Decisions/0029-m18-contract.md decision 40: the hang that stays fatal), however fast the GPU finished: the
			// process ends with GpuHang during the run's third frame, without waiting the budget's 10 s or the reset wait in wall
			// time.
			if (!Test::ProbeGpuForProcess())
				return;
			Test::TempDirectory userData("EditorHang");
			const Result<ProcessResult> result =
				RunRenderingEditor(userData, { "--headless", "--frames", "10", "--gpu-inject-fault", "hang" });
			REQUIRE_MESSAGE(result.has_value(), result.error().ToString());
			INFO("editor stderr: ", result->StandardError);
			CHECK(result->ExitCode == ExitCode::Crash);
			const Result<std::string> report = Test::ReadOnlyCrashReport(userData.GetPath());
			REQUIRE_MESSAGE(report.has_value(), report.error().ToString());
			CHECK(report->contains("Reason: Fatal error (GpuHang)"));
			CHECK(report->contains("FramePhase: Render"));
			CHECK(Test::FindGpuMessageLines(result->StandardError).empty());
		}

		TEST_CASE("EditorApp: --gpu-inject-fault=oom-texture reports the failed texture and keeps running"
			* doctest::test_suite(Test::GpuSuite))
		{
			// Sampled-only textures fail (the ImGui font atlas here); render targets keep working, so the editor runs its
			// frames, reports the failure once, and never crashes (§8.14 item 7). The editor draws no assets before the scene
			// renderer (M7), so the asset half of the acceptance, the placeholder plus an ASSET_UPLOAD_FAILED diagnostic, is
			// "GpuResourceCache: an injected texture OOM yields the placeholder and a diagnostic" (ADR 0010 decision 22).
			if (!Test::ProbeGpuForProcess())
				return;
			Test::TempDirectory userData("EditorOomTexture");
			const Result<ProcessResult> result =
				RunRenderingEditor(userData, { "--headless", "--frames", "10", "--gpu-inject-fault", "oom-texture" });
			REQUIRE_MESSAGE(result.has_value(), result.error().ToString());
			INFO("editor stderr: ", result->StandardError);
			CHECK(result->ExitCode == ExitCode::Success);
			CHECK(result->StandardError.contains("cannot create texture"));
			const std::vector<std::filesystem::path> reports = Test::ListCrashFiles(userData / ENGINE_PRODUCT_NAME / "Crashes", ".txt");
			CHECK(reports.empty());
			CHECK(Test::FindGpuMessageLines(result->StandardError).empty());
		}
	}

}
