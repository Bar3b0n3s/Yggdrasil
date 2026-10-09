#pragma once

#include "Engine/Core/Result.h"

#include <filesystem>
#include <optional>
#include <string_view>

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
	};

}
