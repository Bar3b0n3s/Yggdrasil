#pragma once

#include "Engine/Core/Result.h"

#include <filesystem>
#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Engine {

	// Portable ImGui directory picker. Main thread, one instance per dialog, owns path/filter state only.
	// No shell, cwd mutation or OS UI dependency. Enumeration uses FileSystem, byte-wise stable directories-first order.
	class FolderPicker
	{
	public:
		// InvalidArgument empty title; NotFound/Io unreadable initial directory; preserves a previously open picker on error.
		[[nodiscard]] Status Open(std::string_view title, const std::filesystem::path& initialDirectory);
		// nullopt while pending/cancelled; on accept owns a canonical absolute directory. Io is shown and returned.
		[[nodiscard]] Result<std::optional<std::filesystem::path>> Draw();
		void Cancel();
		[[nodiscard]] bool IsOpen() const;
	private:
		[[nodiscard]] Status Navigate(const std::filesystem::path& directory);
		std::string m_Title{};
		std::filesystem::path m_Directory{};
		std::vector<std::filesystem::path> m_Directories{};
		std::array<char, 4096> m_PathInput{};
		std::array<char, 256> m_Filter{};
		std::string m_Error{};
		bool m_Open = false;
	};

}
