#include "EnginePCH.h"
#include "Engine/Core/BuildId.h"

#include "Engine/Core/Assert.h"

#include <algorithm>
#include <format>

// The stamp (Docs/Decisions/0030-m19-contract.md decision 13): Scripts/Build.py writes the commit and dirty flag of the
// working tree into a generated header below the configuration's intermediate directory before it builds, rewriting it
// only when its content changes, so a new commit recompiles this one translation unit. The build names that header with
// ENGINE_BUILD_STAMP_HEADER (premake), and the header defines ENGINE_BUILD_COMMIT (a string literal: 12 lowercase hex
// digits, or empty without git) and ENGINE_BUILD_DIRTY (0 or 1). A build without the stamp (the IDE's, before any Build.py
// run, which never wrote the header) has an empty commit; Build.py's first stamp removes this file's object, so the next
// build compiles it with the stamp. The header is included by a macro, since it lies outside every include root.
#if defined(ENGINE_BUILD_STAMP_HEADER)
	#if __has_include(ENGINE_BUILD_STAMP_HEADER)
		#include ENGINE_BUILD_STAMP_HEADER
	#endif
#endif

namespace Engine {

	namespace {

		// The length of a commit in a build id (BuildId::Commit).
		constexpr size_t CommitDigits = 12;
		// The build metadata of a dirty tree after the commit.
		constexpr std::string_view DirtySuffix = ".dirty";

		// 12 lowercase hex digits.
		[[nodiscard]] constexpr bool IsCommitText(std::string_view text)
		{
			return text.size() == CommitDigits && std::ranges::all_of(text, [](char character)
			{
				return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
			});
		}

		// A decimal number without a sign or leading zeros ("0", "12"; not "", "01", "+1").
		[[nodiscard]] constexpr bool IsVersionNumber(std::string_view text)
		{
			if (text.empty() || (text.size() > 1 && text.front() == '0'))
				return false;
			return std::ranges::all_of(text, [](char character)
			{
				return character >= '0' && character <= '9';
			});
		}

		// MAJOR.MINOR.PATCH, each IsVersionNumber.
		[[nodiscard]] constexpr bool IsVersionText(std::string_view text)
		{
			for (int part = 0; part < 2; ++part)
			{
				const size_t dot = text.find('.');
				if (dot == std::string_view::npos || !IsVersionNumber(text.substr(0, dot)))
					return false;
				text.remove_prefix(dot + 1);
			}
			return IsVersionNumber(text);
		}

		[[maybe_unused]] [[nodiscard]] bool IsValidBuildId(const BuildId& id)
		{
			return IsVersionText(id.Version) && (id.Commit.empty() || IsCommitText(id.Commit)) && (!id.Dirty || !id.Commit.empty());
		}

#if defined(ENGINE_BUILD_COMMIT)
		constexpr std::string_view StampedCommit = ENGINE_BUILD_COMMIT;
		constexpr bool StampedDirty = ENGINE_BUILD_DIRTY != 0;
#else
		constexpr std::string_view StampedCommit{};
		constexpr bool StampedDirty = false;
#endif

		static_assert(StampedCommit.empty() || IsCommitText(StampedCommit),
			"the build stamp's ENGINE_BUILD_COMMIT is not 12 lowercase hex digits: build with Scripts/Build.py, which rewrites it");
		static_assert(IsVersionText(EngineVersionString), "EngineVersionString is MAJOR.MINOR.PATCH");

	}

	const BuildId& GetBuildId()
	{
		// Immutable after its thread-safe initialization.
		static const BuildId Stamped{
			.Version = std::string(EngineVersionString),
			.Commit = std::string(StampedCommit),
			.Dirty = StampedDirty && !StampedCommit.empty(),
		};
		return Stamped;
	}

	std::string FormatBuildId(const BuildId& id)
	{
		ENGINE_CORE_ASSERT(IsValidBuildId(id), "FormatBuildId takes a valid build id, not '{}' '{}' {}", id.Version, id.Commit, id.Dirty);
		if (id.Commit.empty())
			return id.Version;
		return std::format("{}+{}{}", id.Version, id.Commit, id.Dirty ? DirtySuffix : std::string_view());
	}

	Result<BuildId> ParseBuildId(std::string_view text)
	{
		const auto refuse = [text](std::string_view why)
		{
			return std::unexpected(Error(ErrorCode::InvalidArgument, std::format("'{}' is not a build id: {}", text, why))
					.WithHint("a build id is <major>.<minor>.<patch>, optionally followed by +<12 lowercase hex digits of the commit> and .dirty"));
		};
		const size_t plus = text.find('+');
		const std::string_view version = text.substr(0, plus);
		if (!IsVersionText(version))
			return refuse("its version is not three decimal numbers without signs or leading zeros");
		BuildId id{ .Version = std::string(version), .Commit = {}, .Dirty = false };
		if (plus == std::string_view::npos)
			return id;
		std::string_view metadata = text.substr(plus + 1);
		if (metadata.ends_with(DirtySuffix))
		{
			id.Dirty = true;
			metadata.remove_suffix(DirtySuffix.size());
		}
		if (!IsCommitText(metadata))
			return refuse("its build metadata is not a 12-digit lowercase hex commit, optionally followed by .dirty");
		id.Commit = std::string(metadata);
		return id;
	}

}
