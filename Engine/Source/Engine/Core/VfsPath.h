#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"

#include <compare>
#include <cstddef>
#include <format>
#include <functional>
#include <string>
#include <string_view>

namespace Engine {

	// A validated virtual file system path (Architecture §4.10): "<scheme>://<path>", for example
	// "project://Assets/Scenes/Level1.scene". A VfsPath can only be obtained through Parse, Create or Join, so every
	// non-empty instance satisfies these rules:
	//   - scheme: one or more lowercase ASCII letters ("engine", "project", "user", "cache", "enginecache"); whether it
	//     is mounted is the VirtualFileSystem's concern;
	//   - path: empty (the mount root) or segments separated by single forward slashes, each segment non-empty;
	//   - rejected anywhere in the path, as Validation errors whose message names the offending segment: a backslash; a
	//     leading '/' (absolute path) or any ':' (drive letters, alternate data streams); empty segments ("a//b", a
	//     trailing '/'); "." and ".." segments (parent escapes; there is exactly one spelling per file); NUL and the other
	//     control characters U+0001-U+001F; the characters < > " | ? *; a segment ending in '.' or ' '; a Windows
	//     reserved device name, that is a segment whose part before its first '.' (the whole segment when it has none),
	//     with trailing spaces removed, equals CON, PRN, AUX, NUL, COM1-COM9 or LPT1-LPT9 in any letter case ("nul",
	//     "nul.txt", "nul.tar.gz", "CON .txt"); invalid UTF-8.
	// Paths are case-sensitive on every host (case policy, §4.10). Comparison and ordering are byte-wise on the scheme,
	// then the path, which is the canonical order. A value type; thread-compatible.
	class VfsPath
	{
	public:
		// The empty path (IsEmpty()); not accepted by VirtualFileSystem calls.
		VfsPath() = default;

		// Parses "<scheme>://<path>". Errors: Validation (see the class comment), including a missing "://".
		[[nodiscard]] static Result<VfsPath> Parse(std::string_view text);

		// The path `path` (relative, without a scheme) under `scheme`. Errors: Validation.
		[[nodiscard]] static Result<VfsPath> Create(std::string_view scheme, std::string_view path);

		// The scheme and relative-path rules of the class comment, on their own. Errors: Validation.
		[[nodiscard]] static Status ValidateScheme(std::string_view scheme);
		[[nodiscard]] static Status ValidateRelativePath(std::string_view path);

		[[nodiscard]] bool IsEmpty() const { return m_Scheme.empty(); }
		// True for the root of a mount ("project://").
		[[nodiscard]] bool IsRoot() const { return !m_Scheme.empty() && m_Path.empty(); }

		[[nodiscard]] std::string_view GetScheme() const { return m_Scheme; }
		// The part after "://"; empty for a root.
		[[nodiscard]] std::string_view GetPath() const { return m_Path; }

		// "<scheme>://<path>"; empty for the empty path.
		[[nodiscard]] std::string ToString() const;

		// The last segment ("Level1.scene"); empty for a root.
		[[nodiscard]] std::string_view GetFileName() const;
		// The file name from its last '.', included (".scene", ".meta" for "Track.glb.meta"); empty when the name has no
		// '.' or its only '.' is the first character (".luaurc"), as std::filesystem::path::extension.
		[[nodiscard]] std::string_view GetExtension() const;
		// The file name without GetExtension() ("Track.glb" for "Track.glb.meta").
		[[nodiscard]] std::string_view GetStem() const;

		// The containing directory ("project://Assets/Scenes" for "project://Assets/Scenes/Level1.scene"). The parent of
		// a root is the root itself, the parent of the empty path is empty.
		[[nodiscard]] VfsPath GetParent() const;

		// This path followed by `relative`, one or more segments separated by '/', each validated like a path. Joining
		// onto a root gives "<scheme>://<relative>". Errors: Validation; joining onto the empty path is InvalidArgument.
		[[nodiscard]] Result<VfsPath> Join(std::string_view relative) const;

		// True when both have the same scheme and this path equals `directory` or lies below it (segment-wise:
		// "Assets/Scenes2" is not under "Assets/Scenes"). Every path of a scheme is under that scheme's root.
		[[nodiscard]] bool IsUnder(const VfsPath& directory) const;

		std::strong_ordering operator<=>(const VfsPath&) const = default;
		bool operator==(const VfsPath&) const = default;
	private:
		std::string m_Scheme;
		std::string m_Path;
	};

}

template<>
struct std::hash<Engine::VfsPath>
{
	[[nodiscard]] size_t operator()(const Engine::VfsPath& path) const noexcept
	{
		const size_t schemeHash = std::hash<std::string_view>()(path.GetScheme());
		return schemeHash ^ (std::hash<std::string_view>()(path.GetPath()) + 0x9e3779b97f4a7c15ull + (schemeHash << 6) + (schemeHash >> 2));
	}
};

// Formats as VfsPath::ToString(); no format specification is accepted ("{}").
template<>
struct std::formatter<Engine::VfsPath, char>
{
	constexpr std::format_parse_context::iterator parse(std::format_parse_context& context)
	{
		return context.begin();
	}

	template<typename FormatContext>
	auto format(const Engine::VfsPath& path, FormatContext& context) const
	{
		if (path.IsEmpty())
			return context.out();
		return std::format_to(context.out(), "{}://{}", path.GetScheme(), path.GetPath());
	}
};
