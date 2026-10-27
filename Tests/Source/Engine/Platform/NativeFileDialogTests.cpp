#include "TestsPCH.h"

#include "Engine/Platform/NativeFileDialog.h"

#include "Engine/Platform/Window.h"
#include "Support/TempDirectory.h"
#include "Support/TestOptions.h"
#include "Support/WindowedChild.h"

// The native file dialogs' platform layer (M18, C18.1, E-DIALOGS; Docs/Decisions/0029-m18-contract.md decisions 19 to 21).
// No test waits for a user (ShowErrorDialog's rule): the request rules are tested on every platform, Linux's and macOS's
// refusal where it is compiled, and on Windows a windowed child shows a real dialog over a real window and closes it with
// Cancel.

namespace Engine {

	namespace {

		// A valid OpenFile request starting in the test's temporary directory.
		FileDialogRequest MakeOpenProjectRequest(const std::filesystem::path& directory)
		{
			return { .Kind = FileDialogKind::OpenFile, .Title = "Open a project", .InitialDirectory = directory, .Extensions = { ".eproj" }, .FilterName = "Projects" };
		}

	}

	TEST_SUITE("Platform")
	{
		TEST_CASE("NativeFileDialog: a malformed request is refused before any dialog is shown")
		{
			Test::TempDirectory directory("NativeFileDialogRequest");
			const FileDialogRequest valid = MakeOpenProjectRequest(directory.GetPath());
			REQUIRE(ValidateFileDialogRequest(valid).has_value());
			FileDialogRequest scripts = valid;
			scripts.Extensions = { ".luau", ".test.luau" };
			CHECK(ValidateFileDialogRequest(scripts).has_value());
			FileDialogRequest folder = { .Kind = FileDialogKind::PickFolder, .Title = "Choose a location", .InitialDirectory = directory.GetPath(), .Extensions = {}, .FilterName = {} };
			CHECK(ValidateFileDialogRequest(folder).has_value());
			// A directory that does not exist is no request error: the FileDialog checks it, and a native dialog starts in its
			// default folder.
			FileDialogRequest absent = valid;
			absent.InitialDirectory = directory / "Absent";
			CHECK(ValidateFileDialogRequest(absent).has_value());

			std::vector<FileDialogRequest> malformed;
			malformed.push_back(valid);
			malformed.back().Title.clear();
			malformed.push_back(valid);
			malformed.back().Title = std::string(MaxFileDialogTitleBytes + 1, 'a');
			malformed.push_back(valid);
			malformed.back().Title = "Line\nbreak";
			malformed.push_back(valid);
			malformed.back().Title = std::string("\xff\xfe", 2);
			malformed.push_back(valid);
			malformed.back().InitialDirectory = "Relative/Folder";
			malformed.push_back(valid);
			malformed.back().InitialDirectory.clear();
			for (const char* extension : { "eproj", ".", ".EPROJ", ".e proj", "..eproj", ".eproj.", ".a/b" })
			{
				malformed.push_back(valid);
				malformed.back().Extensions = { extension };
			}
			malformed.push_back(valid);
			malformed.back().Extensions = { ".eproj", ".eproj" };
			malformed.push_back(valid);
			malformed.back().Extensions = { "." + std::string(MaxFileDialogExtensionBytes, 'a') };
			malformed.push_back(valid);
			malformed.back().Extensions.assign(MaxFileDialogExtensions + 1, ".a");
			for (size_t index = 0; index <= MaxFileDialogExtensions; ++index)
				malformed.back().Extensions[index] = std::format(".x{}", index);
			malformed.push_back(valid);
			malformed.back().FilterName = std::string(MaxFileDialogFilterNameBytes + 1, 'a');
			malformed.push_back(folder);
			malformed.back().Extensions = { ".eproj" };
			malformed.push_back(folder);
			malformed.back().FilterName = "Folders";
			for (size_t index = 0; index < malformed.size(); ++index)
			{
				CAPTURE(index);
				const Status status = ValidateFileDialogRequest(malformed[index]);
				REQUIRE_FALSE(status.has_value());
				CHECK(status.error().GetCode() == ErrorCode::InvalidArgument);
				// Open refuses it the same way on every platform, and shows nothing.
				const Result<Scope<NativeFileDialog>> opened = NativeFileDialog::Open(malformed[index]);
				REQUIRE_FALSE(opened.has_value());
				CHECK(opened.error().GetCode() == ErrorCode::InvalidArgument);
			}
		}

#if defined(ENGINE_PLATFORM_WINDOWS)
		// Runs only in a windowed child process (Support/WindowedChild.h): the dialog runs on its own thread, so this one keeps
		// pumping its window's events while the dialog is pending, and Cancel closes it without a user.
		TEST_CASE("NativeFileDialog: a dialog shown over a window stays pending until Cancel closes it"
			* doctest::test_suite(Test::ChildTargetSuite) * doctest::skip(true))
		{
			Test::TempDirectory directory("NativeFileDialogShown");
			Result<Window> window = Window::Create({ .Title = "NativeFileDialog", .Width = 320, .Height = 240 });
			REQUIRE_MESSAGE(window.has_value(), window.error().ToString());
			Result<Scope<NativeFileDialog>> dialog = NativeFileDialog::Open(MakeOpenProjectRequest(directory.GetPath()), &*window);
			REQUIRE_MESSAGE(dialog.has_value(), dialog.error().ToString());
			for (int frame = 0; frame < 10; ++frame)
			{
				window->PollEvents();
				const Result<FileDialogOutcome> pending = (*dialog)->Poll();
				REQUIRE_MESSAGE(pending.has_value(), pending.error().ToString());
				CHECK(pending->State == FileDialogState::Pending);
			}
			(*dialog)->Cancel();
			const Result<FileDialogOutcome> cancelled = (*dialog)->Poll();
			REQUIRE_MESSAGE(cancelled.has_value(), cancelled.error().ToString());
			CHECK(cancelled->State == FileDialogState::Cancelled);
			CHECK(cancelled->Path.empty());
			// Idempotent; the outcome stays.
			(*dialog)->Cancel();
			CHECK((*dialog)->Poll().value_or(FileDialogOutcome{}).State == FileDialogState::Cancelled);
			// A folder picker over no window closes the same way.
			const FileDialogRequest folder = { .Kind = FileDialogKind::PickFolder, .Title = "Choose a location", .InitialDirectory = directory.GetPath(), .Extensions = {}, .FilterName = {} };
			Result<Scope<NativeFileDialog>> unowned = NativeFileDialog::Open(folder);
			REQUIRE_MESSAGE(unowned.has_value(), unowned.error().ToString());
			unowned->reset();
		}

		TEST_CASE("NativeFileDialog: a native dialog runs on its own thread and closes on Cancel")
		{
			ENGINE_CHECK_WINDOWED_CHILD("NativeFileDialog: a dialog shown over a window stays pending until Cancel closes it");
		}
#else
		TEST_CASE("NativeFileDialog: Linux and macOS have no native dialog")
		{
			// Windows backends only (ADR 0028 decision 10): the FileDialog shows its ImGui picker here.
			Test::TempDirectory directory("NativeFileDialogUnsupported");
			const Result<Scope<NativeFileDialog>> opened = NativeFileDialog::Open(MakeOpenProjectRequest(directory.GetPath()));
			REQUIRE_FALSE(opened.has_value());
			CHECK(opened.error().GetCode() == ErrorCode::Unsupported);
		}
#endif
	}

}
