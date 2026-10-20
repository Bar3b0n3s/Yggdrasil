#include "EnginePCH.h"
#include "Engine/Platform/NativeFileDialog.h"

#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Utf8.h"

#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The platform-independent part of the native file dialogs: the request rules both of the editor's dialogs share
// (Docs/Decisions/0029-m18-contract.md decision 19). The dialogs themselves live in Platform/Windows/ and Platform/Posix/.

namespace Engine {

	namespace Utils {

		// Whether `text` (already valid UTF-8) contains a C0 or C1 control character or DEL.
		static bool ContainsDialogControlCharacter(std::string_view text)
		{
			for (size_t index = 0; index < text.size(); ++index)
			{
				const auto byte = static_cast<uint8_t>(text[index]);
				if (byte < 0x20 || byte == 0x7f)
					return true;
				// U+0080 to U+009F are encoded as C2 80 to C2 9F.
				if (byte == 0xc2 && index + 1 < text.size())
				{
					const auto next = static_cast<uint8_t>(text[index + 1]);
					if (next >= 0x80 && next <= 0x9f)
						return true;
				}
			}
			return false;
		}

		// The rules of the request's text fields: at most `maxBytes` bytes of valid UTF-8 without control characters, and
		// not empty when `required`.
		static Status ValidateDialogText(std::string_view field, std::string_view text, size_t maxBytes, bool required)
		{
			if (required && text.empty())
				return MakeError(ErrorCode::InvalidArgument, "the file dialog's {} is empty", field);
			if (text.size() > maxBytes)
				return MakeError(ErrorCode::InvalidArgument, "the file dialog's {} is {} bytes long; the limit is {}", field, text.size(), maxBytes);
			const size_t invalid = FindInvalidUtf8(text);
			if (invalid != text.size())
				return MakeError(ErrorCode::InvalidArgument, "the file dialog's {} is not valid UTF-8 (invalid byte at offset {})", field, invalid);
			if (ContainsDialogControlCharacter(text))
				return MakeError(ErrorCode::InvalidArgument, "the file dialog's {} contains a control character", field);
			return {};
		}

		// Whether `extension` is a '.' followed by one or more '.'-separated runs of lower-case ASCII letters and digits.
		static bool IsDialogExtension(std::string_view extension)
		{
			if (extension.size() < 2 || extension.front() != '.')
				return false;
			bool inRun = false;
			for (const char character : extension.substr(1))
			{
				if (character == '.')
				{
					if (!inRun)
						return false;
					inRun = false;
				}
				else if ((character >= 'a' && character <= 'z') || (character >= '0' && character <= '9'))
					inRun = true;
				else
					return false;
			}
			return inRun;
		}

		// How an error names extension `index`: with its text when that is printable ASCII within the limit, so a message
		// never carries control characters or broken UTF-8.
		static std::string DescribeDialogExtension(const std::vector<std::string>& extensions, size_t index)
		{
			const std::string& extension = extensions[index];
			if (extension.size() > MaxFileDialogExtensionBytes)
				return std::format("extension {}", index);
			for (const char character : extension)
				if (character < 0x20 || character >= 0x7f)
					return std::format("extension {}", index);
			return std::format("extension {} '{}'", index, extension);
		}

	}

	Status ValidateFileDialogRequest(const FileDialogRequest& request)
	{
		if (request.Kind != FileDialogKind::OpenFile && request.Kind != FileDialogKind::PickFolder)
			return MakeError(ErrorCode::InvalidArgument, "the file dialog's kind {} is neither OpenFile nor PickFolder", std::to_underlying(request.Kind));
		ENGINE_TRY(Utils::ValidateDialogText("title", request.Title, MaxFileDialogTitleBytes, true));
		if (request.InitialDirectory.empty() || !request.InitialDirectory.is_absolute())
		{
			return MakeError(ErrorCode::InvalidArgument, "the file dialog's initial folder '{}' is not an absolute path",
				FileSystem::PathToUtf8(request.InitialDirectory));
		}

		if (request.Kind == FileDialogKind::PickFolder)
		{
			if (!request.Extensions.empty())
				return MakeError(ErrorCode::InvalidArgument, "a folder dialog takes no extensions ({} given)", request.Extensions.size());
			if (!request.FilterName.empty())
				return MakeError(ErrorCode::InvalidArgument, "a folder dialog takes no filter name");
			return {};
		}

		if (request.Extensions.size() > MaxFileDialogExtensions)
		{
			return MakeError(ErrorCode::InvalidArgument, "the file dialog lists {} extensions; the limit is {}", request.Extensions.size(),
				MaxFileDialogExtensions);
		}
		for (size_t index = 0; index < request.Extensions.size(); ++index)
		{
			const std::string& extension = request.Extensions[index];
			if (extension.size() > MaxFileDialogExtensionBytes)
			{
				return MakeError(ErrorCode::InvalidArgument, "the file dialog's {} is {} bytes long; the limit is {}",
					Utils::DescribeDialogExtension(request.Extensions, index), extension.size(), MaxFileDialogExtensionBytes);
			}
			if (!Utils::IsDialogExtension(extension))
			{
				return MakeError(ErrorCode::InvalidArgument,
					"the file dialog's {} is not a '.' followed by '.'-separated runs of lower-case ASCII letters and digits, such as .eproj or .test.luau",
					Utils::DescribeDialogExtension(request.Extensions, index));
			}
			for (size_t earlier = 0; earlier < index; ++earlier)
			{
				if (request.Extensions[earlier] == extension)
				{
					return MakeError(ErrorCode::InvalidArgument, "the file dialog lists the extension '{}' twice (entries {} and {})", extension,
						earlier, index);
				}
			}
		}
		return Utils::ValidateDialogText("filter name", request.FilterName, MaxFileDialogFilterNameBytes, false);
	}

}
