#include "EnginePCH.h"
#include "Engine/Platform/Paths.h"

#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Utf8.h"
#include "Engine/Platform/Private/PathsUtf8.h"

// The host-independent part of Paths: name validation, the folder layout and folder creation. GetUserDataRoot lives in
// Platform/Windows/PathsWindows.cpp and Platform/Posix/PathsPosix.cpp.

namespace Engine {

	namespace Utils {

		// Characters that Windows forbids in a file name; '/' and '\\' also separate folders everywhere.
		static constexpr std::string_view ForbiddenAppNameCharacters = "<>:\"/\\|?*";

		// Names Windows reserves for devices, with or without an extension (Microsoft's "Naming Files, Paths, and
		// Namespaces"), as Paths.h lists them.
		static constexpr std::array<std::string_view, 6> ReservedDeviceNames = { "CON", "PRN", "AUX", "NUL", "CONIN$", "CONOUT$" };
		static constexpr std::array<std::string_view, 2> ReservedDevicePrefixes = { "COM", "LPT" };
		// ASCII digits, then the UTF-8 encodings of the superscripts one, two and three.
		static constexpr std::array<std::string_view, 13> ReservedDeviceDigits = {
			"0",
			"1",
			"2",
			"3",
			"4",
			"5",
			"6",
			"7",
			"8",
			"9",
			"\xc2\xb9",
			"\xc2\xb2",
			"\xc2\xb3",
		};

		static char ToAsciiLower(char character)
		{
			return character >= 'A' && character <= 'Z' ? static_cast<char>(character - 'A' + 'a') : character;
		}

		// Device names match in any case, but only 'A'-'Z' fold: Windows compares names with its own case table, which
		// folds no byte of a UTF-8 sequence into ASCII.
		static bool EqualsIgnoreAsciiCase(std::string_view lhs, std::string_view rhs)
		{
			return std::ranges::equal(lhs, rhs, [](char left, char right)
			{
				return ToAsciiLower(left) == ToAsciiLower(right);
			});
		}

		// Whether `name` (already valid UTF-8) contains a C0 or C1 control character or DEL.
		static bool ContainsControlCharacter(std::string_view name)
		{
			for (size_t index = 0; index < name.size(); ++index)
			{
				const auto byte = static_cast<uint8_t>(name[index]);
				if (byte < 0x20 || byte == 0x7f)
					return true;
				// U+0080 to U+009F are encoded as C2 80 to C2 9F.
				if (byte == 0xc2 && index + 1 < name.size())
				{
					const auto next = static_cast<uint8_t>(name[index + 1]);
					if (next >= 0x80 && next <= 0x9f)
						return true;
				}
			}
			return false;
		}

		// Whether Windows treats `name` as a device: the part before the first '.', without trailing spaces, is a reserved
		// device name in any case.
		static bool IsReservedDeviceName(std::string_view name)
		{
			std::string_view base = name.substr(0, name.find('.'));
			while (!base.empty() && base.back() == ' ')
				base.remove_suffix(1);

			for (const std::string_view reserved : ReservedDeviceNames)
			{
				if (EqualsIgnoreAsciiCase(base, reserved))
					return true;
			}
			for (const std::string_view prefix : ReservedDevicePrefixes)
			{
				if (base.size() <= prefix.size() || !EqualsIgnoreAsciiCase(base.substr(0, prefix.size()), prefix))
					continue;
				const std::string_view digit = base.substr(prefix.size());
				if (std::ranges::find(ReservedDeviceDigits, digit) != ReservedDeviceDigits.end())
					return true;
			}
			return false;
		}

	}

	std::filesystem::path UserDataPaths::GetLogFile(std::string_view executableName) const
	{
		std::string fileName(executableName);
		fileName += ".log";
		return Logs / Utils::NativePathFromUtf8(fileName);
	}

	Status Paths::ValidateAppName(std::string_view appName)
	{
		if (appName.empty())
			return MakeError(ErrorCode::Validation, "the application name is empty");
		if (appName.size() > MaxAppNameLength)
		{
			return MakeError(ErrorCode::Validation, "the application name is {} bytes long; the limit is {}", appName.size(),
				MaxAppNameLength);
		}
		const size_t invalidOffset = FindInvalidUtf8(appName);
		if (invalidOffset != appName.size())
			return MakeError(ErrorCode::Validation, "the application name is not valid UTF-8 (invalid byte at offset {})", invalidOffset);
		if (Utils::ContainsControlCharacter(appName))
			return MakeError(ErrorCode::Validation, "the application name contains a control character");

		const size_t forbidden = appName.find_first_of(Utils::ForbiddenAppNameCharacters);
		if (forbidden != std::string_view::npos)
		{
			return MakeError(ErrorCode::Validation, "the application name '{}' contains '{}', which folder names cannot contain",
				appName, appName[forbidden]);
		}
		if (appName == "." || appName == "..")
			return MakeError(ErrorCode::Validation, "the application name '{}' names a relative folder", appName);
		if (appName.front() == ' ' || appName.back() == ' ')
			return MakeError(ErrorCode::Validation, "the application name '{}' starts or ends with a space", appName);
		if (appName.back() == '.')
			return MakeError(ErrorCode::Validation, "the application name '{}' ends with a dot", appName);
		if (Utils::IsReservedDeviceName(appName))
			return MakeError(ErrorCode::Validation, "the application name '{}' is a device name reserved by Windows", appName);
		return {};
	}

	Result<UserDataPaths> Paths::GetUserDataPaths(std::string_view appName, const std::filesystem::path& root)
	{
		ENGINE_TRY(ValidateAppName(appName));

		std::filesystem::path userDataRoot = root;
		if (userDataRoot.empty())
		{
			ENGINE_TRY_ASSIGN(userDataRoot, GetUserDataRoot());
		}
		else if (!userDataRoot.is_absolute())
		{
			return MakeError(ErrorCode::InvalidArgument, "the user-data root '{}' is not an absolute path",
				Utils::NativePathToUtf8(userDataRoot));
		}

		UserDataPaths paths;
		paths.Root = userDataRoot / Utils::NativePathFromUtf8(appName);
		paths.Logs = paths.Root / "Logs";
		paths.Crashes = paths.Root / "Crashes";
		return paths;
	}

	Status Paths::CreateUserDataDirectories(const UserDataPaths& paths)
	{
		for (const std::filesystem::path* directory : { &paths.Root, &paths.Logs, &paths.Crashes })
		{
			const Status created = FileSystem::CreateDirectories(*directory);
			if (!created.has_value())
			{
				return MakeError(ErrorCode::Io, "cannot create the user-data folder '{}': {}", Utils::NativePathToUtf8(*directory),
					created.error().GetMessageText());
			}
		}
		return {};
	}

}
