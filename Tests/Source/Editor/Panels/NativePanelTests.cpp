#include "TestsPCH.h"
#include "Editor/Panels/NativePanel.h"

#include "Editor/SupportingPanelTestUi.h"
#include "EditorCore/EditorPreferences.h"
#include "EditorCore/Native/NativeBuild.h"
#include "EditorCore/Native/NativeModuleHost.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Json/JsonWriter.h"
#include "Engine/Core/VfsPath.h"
#include "Engine/Core/VirtualFileSystem.h"
#include "Engine/Platform/SharedLibrary.h"
#include "Support/ExpectLog.h"
#include "Support/FakeNativeBackends.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace Engine {

	namespace {

		void WriteNativePanelFile(const std::filesystem::path& path, std::string_view text)
		{
			REQUIRE(FileSystem::CreateDirectories(path.parent_path()).has_value());
			REQUIRE(FileSystem::WriteFileAtomic(path, std::as_bytes(std::span(text.data(), text.size()))).has_value());
		}

		// The user's checkbox, applied by the editor's next frame (EditorContext::Update), as in the editor.
		void AllowNativeCode(Test::UtilityPanelFixture& fixture)
		{
			fixture.GetEditor().GetNativeModule().QueuePreferenceChange(true);
			fixture.GetEditor().Update(0.0);
			REQUIRE(fixture.GetEditor().GetNativeModule().IsAllowed());
		}

		// A project whose Native/ holds a source, with a successful module build of it as Scripts/BuildNative.py leaves one
		// (its record and its library), so the panel offers Load without running a toolchain.
		void WriteNativeBuild(Test::UtilityPanelFixture& fixture)
		{
			const std::filesystem::path root = fixture.GetEditor().GetProject().GetRoot();
			WriteNativePanelFile(root / "Native" / "Spinner.cpp", "#include \"Engine/Native/NativeApi.h\"\n");
			const NativeModuleHostSpecification specification = MakeEditorNativeModuleHostSpecification(nullptr, nullptr);
			const Result<std::string> hash = ComputeNativeSourceHash(root, specification.EditorConfiguration, specification.ApiHeader);
			REQUIRE_MESSAGE(hash.has_value(), hash.error().ToString());
			const std::filesystem::path build = root / "Library" / "Native" / "Builds" / "00000001";
			WriteNativePanelFile(build / "Module.dll", "not a library\n");
			WriteNativePanelFile(build / "Build.log", "built\n");
			Json record = Json::object();
			record["Format"] = "NativeBuild";
			record["Version"] = 1;
			record["Kind"] = "Module";
			record["Configuration"] = std::string(ExportConfigurationToString(specification.EditorConfiguration));
			record["SourceHash"] = *hash;
			record["Sources"] = Json::array({ "Native/Spinner.cpp" });
			record["Succeeded"] = true;
			record["Output"] = "Module.dll";
			record["Log"] = "Build.log";
			record["Diagnostics"] = Json::array();
			const Result<std::string> text = JsonWriter::Write(record);
			REQUIRE(text.has_value());
			WriteNativePanelFile(build / "NativeBuild.json", *text);
		}

	}

	TEST_SUITE("Editor")
	{
		// Docs/Decisions/0030-m19-contract.md decision 29 (N-C): the checkbox is the preference's only writer, and it only
		// queues the change: the editor's next frame saves it into user://Editor.json and applies it.
		TEST_CASE("NativePanel: the Allow native code checkbox queues the preference, which the editor's next frame saves")
		{
			Test::UtilityPanelFixture fixture;
			fixture.OpenProject();
			NativePanel panel;
			NativeModuleHost& host = fixture.GetEditor().GetNativeModule();
			CHECK(fixture.Draw(panel).contains("Native code is off"));
			Test::SupportingPanelTestUi::ClickText(fixture, panel, "Allow native code");
			CHECK_FALSE(host.IsAllowed());
			const Result<EditorPreferences> queued = ReadEditorPreferences(fixture.GetEditor().GetVfs());
			REQUIRE(queued.has_value());
			CHECK_FALSE(queued->AllowNativeCode);

			fixture.GetEditor().Update(0.0);
			CHECK(host.IsAllowed());
			const Result<EditorPreferences> saved = ReadEditorPreferences(fixture.GetEditor().GetVfs());
			REQUIRE(saved.has_value());
			CHECK(saved->AllowNativeCode);
			CHECK(fixture.Draw(panel).contains("Native code is allowed"));

			Test::SupportingPanelTestUi::ClickText(fixture, panel, "Allow native code");
			fixture.GetEditor().Update(0.0);
			CHECK_FALSE(host.IsAllowed());
			const Result<EditorPreferences> off = ReadEditorPreferences(fixture.GetEditor().GetVfs());
			REQUIRE(off.has_value());
			CHECK_FALSE(off->AllowNativeCode);
		}

		// Decisions 29 and 32 (N-C): the panel's Build and Load are the native.build and native.load requests agents send,
		// queued for the editor's safe point (EditorActions), so the panel has no action of its own.
		TEST_CASE("NativePanel: Build and Load queue native.build and native.load")
		{
			if (!SharedLibrary::CheckSupported().has_value())
				return; // the panel offers neither where the platform has no backend (decision 34)
			Test::UtilityPanelFixture fixture;
			fixture.OpenProject();
			AllowNativeCode(fixture);
			WriteNativeBuild(fixture);
			NativePanel panel;
			CHECK(fixture.Draw(panel).contains("Newest build: 00000001"));

			// Load runs at the safe point and fails here, since the build's library is a text file; the panel says so.
			const uint64_t beforeLoad = fixture.ProbeActionTicket();
			Test::SupportingPanelTestUi::ClickText(fixture, panel, "Load");
			REQUIRE(fixture.FindPendingAction(beforeLoad).has_value());
			fixture.Pump();
			CHECK(fixture.Draw(panel).contains("Loading failed"));
			CHECK(fixture.GetEditor().GetNativeModule().GetLoadedTypes() == nullptr);

			// Build queues a request that would start the toolchain at the safe point: it is withdrawn unrun.
			const uint64_t beforeBuild = fixture.ProbeActionTicket();
			Test::SupportingPanelTestUi::ClickText(fixture, panel, "Build");
			const std::optional<uint64_t> build = fixture.FindPendingAction(beforeBuild);
			REQUIRE(build.has_value());
			REQUIRE(fixture.GetContext().Actions.Discard(*build).has_value());
		}

		// Decision 29 (N-C; the review of W19.2): a change the editor's next frame could not save leaves the preference as it
		// was, and the panel says so once a frame has passed, pointing to the Console's error.
		TEST_CASE("NativePanel: a preference change that cannot be saved is reported and leaves the preference off")
		{
			Test::UtilityPanelFixture fixture;
			fixture.OpenProject();
			EditorContext& editor = fixture.GetEditor();
			VirtualFileSystem& vfs = editor.GetVfs();
			const Result<VfsPath> preferences = VfsPath::Parse("user://Editor.json");
			REQUIRE(preferences.has_value());
			if (vfs.Exists(*preferences))
				REQUIRE(vfs.Remove(*preferences).has_value());
			// A folder where the file goes, which can be neither read nor replaced.
			REQUIRE(vfs.CreateDirectories(*preferences).has_value());
			NativePanel panel;
			Test::SupportingPanelTestUi::ClickText(fixture, panel, "Allow native code");
			CHECK_FALSE(fixture.Draw(panel).contains("could not be saved"));
			{
				const Test::ExpectLog failed(LogLevel::Error, "Allow native code");
				editor.Update(0.0);
			}
			editor.GetUiState().CompleteFrame();
			const std::string shown = fixture.Draw(panel);
			CHECK(shown.contains("could not be saved"));
			CHECK(shown.contains("Native code is off"));
			CHECK_FALSE(editor.GetNativeModule().IsAllowed());
		}

		// Decision 31 (N-C; the review of W19.2): the panel reads the status again whenever the loaded module changed since it
		// last read it, also when the change happened while it was not drawn: it observes the module it saw only weakly, so a
		// module loaded later, wherever it is allocated, is never taken for one already gone.
		TEST_CASE("NativePanel: the status follows a module unloaded and loaded again while the panel was not drawn")
		{
			if (!SharedLibrary::CheckSupported().has_value())
				return; // nothing builds or loads where the platform has no backend (decision 34)
			const Ref<Test::FakeNativeBuildBackend> builds = CreateRef<Test::FakeNativeBuildBackend>();
			builds->SetApiHeader(MakeEditorNativeModuleHostSpecification(nullptr, nullptr).ApiHeader);
			Test::UtilityPanelFixture fixture(Test::UtilityPanelClock::Real,
				Test::EditorTestNativeBackends{ .Build = builds, .Loader = CreateRef<Test::FakeNativeLibraryLoader>() });
			fixture.OpenProject();
			AllowNativeCode(fixture);
			EditorContext& editor = fixture.GetEditor();
			NativeModuleHost& host = editor.GetNativeModule();
			const std::filesystem::path source = editor.GetProject().GetRoot() / "Native" / "Spinner.cpp";
			const auto buildAndLoad = [&host, &editor]()
			{
				REQUIRE(host.StartBuild(editor).has_value());
				const std::optional<Result<NativeBuildOutcome>> built = host.PollBuild(editor); // the fake resolves at once
				REQUIRE(built.has_value());
				REQUIRE(built->has_value());
				REQUIRE((*built)->Succeeded);
				const Result<NativeLoadReport> loaded = host.Load(editor);
				REQUIRE_MESSAGE(loaded.has_value(), loaded.error().ToString());
			};
			WriteNativePanelFile(source, "#include \"Engine/Native/NativeApi.h\"\n");
			buildAndLoad();
			NativePanel panel;
			CHECK(fixture.Draw(panel).contains("Newest build: 00000001"));

			// Not drawn meanwhile: the module goes with the preference, the sources change, and a build of them loads.
			host.QueuePreferenceChange(false);
			editor.Update(0.0);
			REQUIRE(host.GetLoadedTypes() == nullptr);
			AllowNativeCode(fixture);
			WriteNativePanelFile(source, "#include \"Engine/Native/NativeApi.h\"\n// changed\n");
			buildAndLoad();
			const std::string shown = fixture.Draw(panel);
			CHECK(shown.contains("Newest build: 00000002"));
			CHECK_FALSE(shown.contains("changed since"));
		}
	}

}
