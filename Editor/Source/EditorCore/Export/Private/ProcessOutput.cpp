#include "EditorPCH.h"
#include "EditorCore/Export/Private/ProcessOutput.h"

#include "Engine/Platform/Process.h"

#include <string_view>
#include <vector>

namespace Engine {

	namespace Utils {

		std::string TailOfProcessOutput(const ProcessResult& result, size_t lineCount)
		{
			std::vector<std::string_view> lines;
			for (const std::string* stream : { &result.StandardOutput, &result.StandardError })
			{
				std::string_view text = *stream;
				while (!text.empty())
				{
					const size_t end = text.find('\n');
					std::string_view line = text.substr(0, end);
					while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
						line.remove_suffix(1);
					if (!line.empty())
						lines.push_back(line);
					if (end == std::string_view::npos)
						break;
					text.remove_prefix(end + 1);
				}
			}
			const size_t first = lines.size() > lineCount ? lines.size() - lineCount : 0;
			std::string tail;
			for (size_t index = first; index < lines.size(); ++index)
			{
				tail += tail.empty() ? "" : "\n";
				tail += lines[index];
			}
			return tail.empty() ? std::string("(no output)") : tail;
		}

	}

}
