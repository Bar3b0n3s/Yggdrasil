#include "EnginePCH.h"
#include "Engine/Platform/Paths.h"

// Paths::GetUserDataRoot on Windows: the user's local application-data folder (FOLDERID_LocalAppData, %LOCALAPPDATA%).

#if defined(ENGINE_PLATFORM_WINDOWS)

	#include "Engine/Platform/Private/PathsUtf8.h"

	#include <windows.h>
	#include <knownfolders.h>
	#include <shlobj.h>

namespace Engine {

	namespace {

		// Frees a string allocated by the shell (SHGetKnownFolderPath) on destruction.
		class ScopedCoTaskString
		{
		public:
			ScopedCoTaskString() = default;

			~ScopedCoTaskString()
			{
				CoTaskMemFree(m_Text);
			}

			ScopedCoTaskString(const ScopedCoTaskString&) = delete;
			ScopedCoTaskString& operator=(const ScopedCoTaskString&) = delete;
			ScopedCoTaskString(ScopedCoTaskString&&) = delete;
			ScopedCoTaskString& operator=(ScopedCoTaskString&&) = delete;

			[[nodiscard]] PWSTR Get() const { return m_Text; }
			[[nodiscard]] PWSTR* Out() { return &m_Text; }
		private:
			PWSTR m_Text = nullptr;
		};

	}

	Result<std::filesystem::path> Paths::GetUserDataRoot()
	{
		ScopedCoTaskString folder;
		const HRESULT result = SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, folder.Out());
		if (FAILED(result) || folder.Get() == nullptr)
		{
			return MakeError(ErrorCode::NotFound, "SHGetKnownFolderPath(FOLDERID_LocalAppData) failed with HRESULT 0x{:08x}",
				static_cast<uint32_t>(result));
		}

		std::filesystem::path root(folder.Get());
		if (!root.is_absolute())
		{
			return MakeError(ErrorCode::NotFound, "the local application-data folder '{}' is not an absolute path",
				Utils::NativePathToUtf8(root));
		}
		return root;
	}

}

#endif
