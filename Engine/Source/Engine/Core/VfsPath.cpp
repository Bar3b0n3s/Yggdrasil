#include "EnginePCH.h"
#include "Engine/Core/VfsPath.h"

#include "Engine/Core/Utf8.h"

namespace Engine {

	namespace Utils {

		static constexpr std::string_view SchemeSeparator = "://";

		static constexpr std::array<std::string_view, 22> ReservedDeviceNames = {
			"CON",
			"PRN",
			"AUX",
			"NUL",
			"COM1",
			"COM2",
			"COM3",
			"COM4",
			"COM5",
			"COM6",
			"COM7",
			"COM8",
			"COM9",
			"LPT1",
			"LPT2",
			"LPT3",
			"LPT4",
			"LPT5",
			"LPT6",
			"LPT7",
			"LPT8",
			"LPT9",
		};

		static char ToAsciiUpper(char character)
		{
			return character >= 'a' && character <= 'z' ? static_cast<char>(character - 'a' + 'A') : character;
		}

		// `text` for an error message: control characters are written as \xNN so the message stays printable. The text is
		// valid UTF-8 (checked before any segment is quoted), so the message is too.
		static std::string Printable(std::string_view text)
		{
			std::string printable;
			printable.reserve(text.size());
			for (const char character : text)
			{
				const auto value = static_cast<uint8_t>(character);
				if (value < 0x20 || value == 0x7f)
					printable += std::format("\\x{:02x}", value);
				else
					printable += character;
			}
			return printable;
		}

		// True for a Windows device name: the part before the first '.', without trailing spaces, is CON, PRN, AUX, NUL,
		// COM1-COM9 or LPT1-LPT9 in any letter case.
		static bool IsReservedDeviceName(std::string_view segment)
		{
			std::string_view base = segment.substr(0, segment.find('.'));
			while (!base.empty() && base.back() == ' ')
				base.remove_suffix(1);

			for (const std::string_view reserved : ReservedDeviceNames)
			{
				if (base.size() == reserved.size()
					&& std::ranges::equal(base, reserved, [](char lhs, char rhs)
				{
					return ToAsciiUpper(lhs) == rhs;
				}))
				{
					return true;
				}
			}
			return false;
		}

		static Status SegmentError(std::string_view path, std::string_view segment, std::string_view reason)
		{
			return MakeError(ErrorCode::Validation, "invalid VFS path '{}': segment '{}' {}", Printable(path), Printable(segment),
				reason);
		}

		static Status ValidateSegment(std::string_view path, std::string_view segment)
		{
			if (segment.empty())
				return MakeError(ErrorCode::Validation, "invalid VFS path '{}': empty segment (a doubled or trailing '/')", Printable(path));
			if (segment == "." || segment == "..")
				return SegmentError(path, segment, "is a relative reference; VFS paths have one spelling and never leave their mount");

			for (const char character : segment)
			{
				const auto value = static_cast<uint8_t>(character);
				if (value < 0x20)
					return SegmentError(path, segment, "contains a control character");
				switch (character)
				{
					case '\\': return SegmentError(path, segment, "contains a backslash; VFS paths separate segments with '/'");
					case ':':  return SegmentError(path, segment, "contains ':' (drive letters and alternate streams are not allowed)");
					case '<':
					case '>':
					case '"':
					case '|':
					case '?':
					case '*':
						return SegmentError(path, segment, std::format("contains the reserved character '{}'", character));
					default:
						break;
				}
			}

			if (segment.back() == '.' || segment.back() == ' ')
				return SegmentError(path, segment, "ends with '.' or ' ', which Windows silently drops");
			if (IsReservedDeviceName(segment))
				return SegmentError(path, segment, "is a reserved Windows device name");
			return {};
		}

		// Validates a relative path. An empty path is the mount root and valid when `allowEmpty`.
		static Status ValidateRelative(std::string_view path, bool allowEmpty)
		{
			if (path.empty())
			{
				if (allowEmpty)
					return {};
				return MakeError(ErrorCode::Validation, "invalid VFS path: the relative path is empty");
			}

			const size_t invalidOffset = FindInvalidUtf8(path);
			if (invalidOffset != path.size())
				return MakeError(ErrorCode::Validation, "invalid VFS path: invalid UTF-8 at byte offset {}", invalidOffset);
			if (path.front() == '/')
				return MakeError(ErrorCode::Validation, "invalid VFS path '{}': absolute paths are not allowed", Printable(path));

			size_t start = 0;
			while (true)
			{
				const size_t end = path.find('/', start);
				const std::string_view segment = path.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
				ENGINE_TRY(ValidateSegment(path, segment));
				if (end == std::string_view::npos)
					break;
				start = end + 1;
			}
			return {};
		}

	}

	Result<VfsPath> VfsPath::Parse(std::string_view text)
	{
		const size_t separator = text.find(Utils::SchemeSeparator);
		if (separator == std::string_view::npos)
		{
			if (!IsValidUtf8(text))
				return MakeError(ErrorCode::Validation, "invalid VFS path: missing '<scheme>://' (and invalid UTF-8)");
			return MakeError(ErrorCode::Validation, "invalid VFS path '{}': missing '<scheme>://'", Utils::Printable(text));
		}
		return Create(text.substr(0, separator), text.substr(separator + Utils::SchemeSeparator.size()));
	}

	Result<VfsPath> VfsPath::Create(std::string_view scheme, std::string_view path)
	{
		ENGINE_TRY(ValidateScheme(scheme));
		ENGINE_TRY(Utils::ValidateRelative(path, true));

		VfsPath result;
		result.m_Scheme = scheme;
		result.m_Path = path;
		return result;
	}

	Status VfsPath::ValidateScheme(std::string_view scheme)
	{
		if (scheme.empty())
			return MakeError(ErrorCode::Validation, "invalid VFS scheme: the scheme is empty");
		const bool isLowercase = std::ranges::all_of(scheme, [](char character)
		{
			return character >= 'a' && character <= 'z';
		});
		if (!isLowercase)
		{
			if (!IsValidUtf8(scheme))
				return MakeError(ErrorCode::Validation, "invalid VFS scheme: schemes are lowercase ASCII letters (got invalid UTF-8)");
			return MakeError(ErrorCode::Validation, "invalid VFS scheme '{}': schemes are lowercase ASCII letters", Utils::Printable(scheme));
		}
		return {};
	}

	Status VfsPath::ValidateRelativePath(std::string_view path)
	{
		return Utils::ValidateRelative(path, true);
	}

	std::string VfsPath::ToString() const
	{
		if (IsEmpty())
			return {};
		std::string text;
		text.reserve(m_Scheme.size() + Utils::SchemeSeparator.size() + m_Path.size());
		text += m_Scheme;
		text += Utils::SchemeSeparator;
		text += m_Path;
		return text;
	}

	std::string_view VfsPath::GetFileName() const
	{
		const std::string_view path = m_Path;
		const size_t slash = path.rfind('/');
		return slash == std::string_view::npos ? path : path.substr(slash + 1);
	}

	std::string_view VfsPath::GetExtension() const
	{
		const std::string_view fileName = GetFileName();
		const size_t dot = fileName.rfind('.');
		if (dot == std::string_view::npos || dot == 0)
			return {};
		return fileName.substr(dot);
	}

	std::string_view VfsPath::GetStem() const
	{
		const std::string_view fileName = GetFileName();
		return fileName.substr(0, fileName.size() - GetExtension().size());
	}

	VfsPath VfsPath::GetParent() const
	{
		VfsPath parent;
		if (IsEmpty())
			return parent;
		parent.m_Scheme = m_Scheme;
		const size_t slash = m_Path.rfind('/');
		if (slash != std::string::npos)
			parent.m_Path = m_Path.substr(0, slash);
		return parent;
	}

	Result<VfsPath> VfsPath::Join(std::string_view relative) const
	{
		if (IsEmpty())
			return MakeError(ErrorCode::InvalidArgument, "cannot join onto the empty VFS path");
		ENGINE_TRY(Utils::ValidateRelative(relative, false));

		VfsPath joined;
		joined.m_Scheme = m_Scheme;
		joined.m_Path.reserve(m_Path.size() + 1 + relative.size());
		joined.m_Path = m_Path;
		if (!joined.m_Path.empty())
			joined.m_Path += '/';
		joined.m_Path += relative;
		return joined;
	}

	bool VfsPath::IsUnder(const VfsPath& directory) const
	{
		if (IsEmpty() || directory.IsEmpty() || m_Scheme != directory.m_Scheme)
			return false;
		if (directory.m_Path.empty() || m_Path == directory.m_Path)
			return true;
		return m_Path.size() > directory.m_Path.size() && m_Path.starts_with(directory.m_Path) && m_Path[directory.m_Path.size()] == '/';
	}

}
