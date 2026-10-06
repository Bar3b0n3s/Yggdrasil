#include "EnginePCH.h"
#include "Engine/Core/Private/PathText.h"

#include "Engine/Core/Private/AsciiText.h"

namespace Engine {

	namespace Utils {

		std::string_view ParentPathOf(std::string_view relativePath)
		{
			const size_t slash = relativePath.rfind('/');
			return slash == std::string_view::npos ? std::string_view() : relativePath.substr(0, slash);
		}

		std::string_view FileNameOf(std::string_view relativePath)
		{
			const size_t slash = relativePath.rfind('/');
			return slash == std::string_view::npos ? relativePath : relativePath.substr(slash + 1);
		}

		std::string JoinRelative(std::string_view directory, std::string_view name)
		{
			std::string joined;
			joined.reserve(directory.size() + 1 + name.size());
			joined += directory;
			if (!directory.empty())
				joined += '/';
			joined += name;
			return joined;
		}

		bool IsStrictlyUnder(std::string_view path, std::string_view directory)
		{
			if (directory.empty())
				return !path.empty();
			return path.size() > directory.size() && path.starts_with(directory) && path[directory.size()] == '/';
		}

		bool IsCaseOnlyRename(std::string_view from, std::string_view to)
		{
			if (ParentPathOf(from) != ParentPathOf(to))
				return false;
			const std::string_view fromName = FileNameOf(from);
			const std::string_view toName = FileNameOf(to);
			return fromName != toName && EqualsIgnoreAsciiCase(fromName, toName);
		}

	}

}
