#include "EditorPCH.h"
#include "Editor/FileDialog.h"

#include "Engine/Core/Assert.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Log.h"

#include <algorithm>
#include <filesystem>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace Engine {

	namespace {

		// A NativeFileDialog behind the IFileDialogSession interface: MakeNativeFileDialogBackend's sessions.
		class NativeFileDialogSession final : public IFileDialogSession
		{
		public:
			explicit NativeFileDialogSession(Scope<NativeFileDialog> dialog)
				: m_Dialog(std::move(dialog))
			{
			}

			[[nodiscard]] Result<FileDialogOutcome> Poll() override { return m_Dialog->Poll(); }

			void Cancel() override { m_Dialog->Cancel(); }
		private:
			Scope<NativeFileDialog> m_Dialog;
		};

	}

	namespace Utils {

		static std::string FoldDialogAsciiCase(std::string text)
		{
			for (char& character : text)
				if (character >= 'A' && character <= 'Z')
					character = static_cast<char>(character + ('a' - 'A'));
			return text;
		}

		// The extensions of a request for a message: ".eproj", ".luau or .test.luau", ".a, .b or .c".
		static std::string DescribeDialogExtensions(const std::vector<std::string>& extensions)
		{
			std::string text;
			for (size_t index = 0; index < extensions.size(); ++index)
			{
				if (index != 0)
					text += index + 1 == extensions.size() ? " or " : ", ";
				text += extensions[index];
			}
			return text;
		}

		// A request's initial folder must be a directory that can be listed, as the ImGui picker needs it (the native dialog
		// starts in its default folder instead of a missing one, but the request is refused the same way for both). Errors:
		// FileSystem::GetInfo's (NotFound for a missing folder) and ListDirectory's (Io for an unreadable one).
		static Status CheckDialogInitialDirectory(const std::filesystem::path& directory)
		{
			ENGINE_TRY_ASSIGN(const FileInfo info, FileSystem::GetInfo(directory));
			if (!info.IsDirectory)
				return MakeError(ErrorCode::NotFound, "'{}' is not a folder to browse", FileSystem::PathToUtf8(directory));
			ENGINE_TRY(FileSystem::ListDirectory(directory));
			return {};
		}

		// A native dialog's answer revalidated as the ImGui picker revalidates its own: an existing absolute path, canonical,
		// a directory for PickFolder, a regular file with one of the request's extensions (ignoring ASCII case) for OpenFile.
		// Errors: NotFound (or the file system's error) when the path does not exist (any more); InvalidArgument for a path
		// of the wrong kind or extension; Io when it cannot be resolved.
		static Result<std::filesystem::path> RevalidateDialogAnswer(const FileDialogRequest& request, const std::filesystem::path& path)
		{
			const std::string text = FileSystem::PathToUtf8(path);
			if (path.empty() || !path.is_absolute())
				return MakeError(ErrorCode::InvalidArgument, "The file dialog returned '{}', which is not an absolute path.", text);
			ENGINE_TRY_ASSIGN(const FileInfo info, FileSystem::GetInfo(path));
			std::error_code error;
			std::filesystem::path canonical = std::filesystem::canonical(path, error);
			if (error)
				return MakeError(ErrorCode::Io, "Cannot open '{}': {}", text, error.message());
			if (request.Kind == FileDialogKind::PickFolder)
			{
				if (!info.IsDirectory)
					return MakeError(ErrorCode::InvalidArgument, "Choose a folder: '{}' is a file.", text);
				return canonical;
			}
			const std::filesystem::file_status status = std::filesystem::status(canonical, error);
			if (error)
				return MakeError(ErrorCode::Io, "Cannot open '{}': {}", text, error.message());
			if (status.type() != std::filesystem::file_type::regular)
				return MakeError(ErrorCode::InvalidArgument, "Choose a file: '{}' is not a file.", text);
			if (!request.Extensions.empty())
			{
				const std::string name = FoldDialogAsciiCase(FileSystem::PathToUtf8(canonical.filename()));
				const bool matches = std::ranges::any_of(request.Extensions, [&name](const std::string& extension)
				{
					return name.ends_with(extension);
				});
				if (!matches)
					return MakeError(ErrorCode::InvalidArgument, "Choose a {} file: '{}' is not one.", DescribeDialogExtensions(request.Extensions), text);
			}
			return canonical;
		}

	}

	FileDialogBackend MakeNativeFileDialogBackend(const Window& owner)
	{
		return [window = &owner](const FileDialogRequest& request) -> Result<Scope<IFileDialogSession>>
		{
			ENGINE_TRY_ASSIGN(Scope<NativeFileDialog> dialog, NativeFileDialog::Open(request, window));
			return Scope<IFileDialogSession>(CreateScope<NativeFileDialogSession>(std::move(dialog)));
		};
	}

	Status FileDialog::Open(const FileDialogRequest& request, const FileDialogBackend& backend)
	{
		// Nothing changes until the request passed both checks and its new dialog opened.
		ENGINE_TRY(ValidateFileDialogRequest(request));
		ENGINE_TRY(Utils::CheckDialogInitialDirectory(request.InitialDirectory));
		if (backend)
		{
			Result<Scope<IFileDialogSession>> session = backend(request);
			if (session)
			{
				ENGINE_ASSERT(*session != nullptr, "a FileDialogBackend returned no session and no error");
				// The new dialog is open before the one it replaces closes, so it replaces it only once it exists.
				Scope<IFileDialogSession> previous = std::exchange(m_Native, std::move(*session));
				if (previous != nullptr)
					previous->Cancel();
				m_Picker.Cancel();
				m_Request = request;
				return {};
			}
			// Unsupported: this platform has no native dialog (Linux, macOS), which is no news. Anything else: the native
			// dialog could not start this time.
			if (session.error().GetCode() != ErrorCode::Unsupported)
				ENGINE_WARN("File dialog: the native dialog cannot start, so the editor's picker shows '{}' instead: {}", request.Title, session.error());
		}
		const Status opened = request.Kind == FileDialogKind::PickFolder
			? m_Picker.Open(request.Title, request.InitialDirectory)
			: m_Picker.OpenFile(request.Title, request.InitialDirectory, std::span<const std::string>(request.Extensions));
		ENGINE_TRY(opened);
		if (m_Native != nullptr)
		{
			m_Native->Cancel();
			m_Native.reset();
		}
		m_Request = request;
		return {};
	}

	Result<std::optional<std::filesystem::path>> FileDialog::Draw()
	{
		if (m_Native == nullptr)
			return m_Picker.Draw();
		Result<FileDialogOutcome> outcome = m_Native->Poll();
		if (outcome && outcome->State == FileDialogState::Pending)
			return std::nullopt;
		// The dialog was answered, cancelled or failed: it is closed before its answer is checked, so an answer is returned
		// once and an error leaves nothing open.
		m_Native.reset();
		if (!outcome)
			return std::unexpected(std::move(outcome).error());
		if (outcome->State == FileDialogState::Cancelled)
			return std::nullopt;
		ENGINE_TRY_ASSIGN(std::filesystem::path path, Utils::RevalidateDialogAnswer(m_Request, outcome->Path));
		return std::optional<std::filesystem::path>(std::move(path));
	}

	void FileDialog::Cancel()
	{
		m_Picker.Cancel();
		if (m_Native != nullptr)
		{
			m_Native->Cancel();
			m_Native.reset();
		}
	}

	bool FileDialog::IsOpen() const
	{
		return m_Native != nullptr || m_Picker.IsOpen();
	}

	bool FileDialog::IsNative() const
	{
		return m_Native != nullptr;
	}

}
