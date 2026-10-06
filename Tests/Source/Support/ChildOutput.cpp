#include "TestsPCH.h"
#include "Support/ChildOutput.h"

#include "Engine/Core/FileSystem.h"
#include "Support/Utf8Path.h"

namespace Engine {

	namespace Test {

		std::string FindBracketedValue(std::string_view text, std::string_view prefix)
		{
			const std::string opening = std::string(prefix) + "[";
			const size_t start = text.find(opening);
			if (start == std::string_view::npos)
				return {};
			const size_t valueStart = start + opening.size();
			const size_t end = text.find(']', valueStart);
			return end == std::string_view::npos ? std::string() : std::string(text.substr(valueStart, end - valueStart));
		}

		bool ContainsInOrder(std::string_view text, std::span<const std::string> parts)
		{
			size_t position = 0;
			for (const std::string& part : parts)
			{
				position = text.find(part, position);
				if (position == std::string_view::npos)
					return false;
				position += part.size();
			}
			return true;
		}

		std::vector<std::filesystem::path> ListCrashFiles(const std::filesystem::path& directory, std::string_view extension)
		{
			std::vector<std::filesystem::path> reports;
			const Result<std::vector<std::filesystem::path>> files = FileSystem::ListDirectory(directory);
			if (!files.has_value())
				return reports;
			for (const std::filesystem::path& file : *files)
			{
				const std::string name = PathToUtf8(file.filename());
				if (name.starts_with("crash-") && name.ends_with(extension))
					reports.push_back(file);
			}
			std::ranges::sort(reports);
			return reports;
		}

		Result<std::string> ReadOnlyCrashReport(const std::filesystem::path& userDataRoot)
		{
			const std::filesystem::path crashes = userDataRoot / ENGINE_PRODUCT_NAME / "Crashes";
			const std::vector<std::filesystem::path> reports = ListCrashFiles(crashes, ".txt");
			if (reports.size() != 1)
				return MakeError(ErrorCode::NotFound, "expected one crash report in '{}', found {}", PathToUtf8(crashes), reports.size());
			return FileSystem::ReadText(reports.front());
		}

	}

}
