#pragma once

#include <filesystem>
#include <string_view>

namespace Engine {

	namespace Test {

		// A per-test temporary directory (CodeStyle §14, Architecture §15.2): created empty and uniquely named as
		// std::filesystem::temp_directory_path() / "EngineTests" / "<label>-<16 random hex digits>", removed with everything in
		// it on destruction. If the directory cannot be created the running test case fails and stops (doctest FAIL), so a test
		// never writes outside its directory. Removal failures are logged as warnings, never thrown. Not copyable or movable.
		class TempDirectory
		{
		public:
			// `label` names the directory for humans (letters, digits, '-' and '_'; asserted).
			explicit TempDirectory(std::string_view label = "Test");
			~TempDirectory();

			TempDirectory(const TempDirectory&) = delete;
			TempDirectory& operator=(const TempDirectory&) = delete;
			TempDirectory(TempDirectory&&) = delete;
			TempDirectory& operator=(TempDirectory&&) = delete;

			// The absolute path of the directory.
			[[nodiscard]] const std::filesystem::path& GetPath() const { return m_Path; }

			// GetPath() / relative, with forward slashes accepted in `relative`.
			[[nodiscard]] std::filesystem::path operator/(std::string_view relative) const;
		private:
			std::filesystem::path m_Path;
		};

	}

}
