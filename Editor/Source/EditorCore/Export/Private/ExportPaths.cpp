#include "EditorPCH.h"
#include "EditorCore/Export/Private/ExportPaths.h"

#include "Engine/Core/VfsPath.h"

#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <vector>

namespace Engine {

	namespace {

		// MatchesExportGlob's matcher, memoized over (pattern position, path position), so "**" and '*' never backtrack
		// exponentially however many of them a pattern holds.
		class GlobMatcher
		{
		public:
			GlobMatcher(std::string_view pattern, std::string_view path)
				: m_Pattern(pattern), m_Path(path), m_Memo((pattern.size() + 1) * (path.size() + 1), Unknown)
			{
			}

			[[nodiscard]] bool Matches(size_t patternIndex, size_t pathIndex)
			{
				const size_t slot = patternIndex * (m_Path.size() + 1) + pathIndex;
				if (m_Memo[slot] == Unknown)
					m_Memo[slot] = Compute(patternIndex, pathIndex) ? Yes : No;
				return m_Memo[slot] == Yes;
			}
		private:
			[[nodiscard]] bool Compute(size_t patternIndex, size_t pathIndex)
			{
				if (patternIndex == m_Pattern.size())
					return pathIndex == m_Path.size();

				const char current = m_Pattern[patternIndex];
				if (current == '*')
				{
					const bool doubleStar = patternIndex + 1 < m_Pattern.size() && m_Pattern[patternIndex + 1] == '*';
					if (doubleStar)
					{
						const size_t next = patternIndex + 2;
						// "**/" also matches no directory at all.
						if (next < m_Pattern.size() && m_Pattern[next] == '/' && Matches(next + 1, pathIndex))
							return true;
						for (size_t end = pathIndex; end <= m_Path.size(); ++end)
						{
							if (Matches(next, end))
								return true;
						}
						return false;
					}
					for (size_t end = pathIndex; end <= m_Path.size(); ++end)
					{
						if (Matches(patternIndex + 1, end))
							return true;
						if (end < m_Path.size() && m_Path[end] == '/')
							return false;
					}
					return false;
				}

				if (pathIndex == m_Path.size())
					return false;
				if (current == '?')
					return m_Path[pathIndex] != '/' && Matches(patternIndex + 1, pathIndex + 1);
				return current == m_Path[pathIndex] && Matches(patternIndex + 1, pathIndex + 1);
			}
		private:
			static constexpr int8_t Unknown = -1;
			static constexpr int8_t No = 0;
			static constexpr int8_t Yes = 1;

			std::string_view m_Pattern;
			std::string_view m_Path;
			std::vector<int8_t> m_Memo;
		};

	}

	namespace Utils {

		Status CheckExportOutputDirectory(std::string_view directory)
		{
			if (directory.empty())
				return {};
			std::string hint = std::format("pass a directory below {0}/, such as \"{0}/Windows-Release/Game\", or leave outDir out for the default", ExportBuildDirectory);
			if (Status valid = VfsPath::ValidateRelativePath(directory); !valid.has_value())
			{
				return std::unexpected(Error(ErrorCode::InvalidArgument,
					std::format("the output directory '{}' is not a valid project-relative path: {}", directory, valid.error().GetMessageText()))
						.WithHint(std::move(hint)));
			}
			const size_t separator = directory.find('/');
			if (separator == std::string_view::npos || directory.substr(0, separator) != ExportBuildDirectory)
			{
				return std::unexpected(Error(ErrorCode::InvalidArgument,
					std::format("the output directory '{}' is not below {}/, the only directory an export writes to", directory, ExportBuildDirectory))
						.WithHint(std::move(hint)));
			}
			return {};
		}

		bool MatchesExportGlob(std::string_view pattern, std::string_view path)
		{
			GlobMatcher matcher(pattern, path);
			return matcher.Matches(0, 0);
		}

	}

}
