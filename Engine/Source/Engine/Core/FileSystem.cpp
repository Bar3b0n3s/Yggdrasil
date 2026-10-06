#include "EnginePCH.h"
#include "Engine/Core/FileSystem.h"

#include "Engine/Core/Assert.h"
#include "Engine/Core/Private/AsciiText.h"
#include "Engine/Core/Private/NativePath.h"
#include "Engine/Core/Private/PathText.h"
#include "Engine/Core/Utf8.h"

#include <cerrno>
#include <chrono>
#include <fstream>
#include <ios>
#include <system_error>
#include <thread>

namespace Engine {

	namespace Utils {

		static constexpr size_t ReadChunkSize = 64 * 1024;
		static constexpr int TemporaryNameAttempts = 16;

		// True for the errors with which a status query reports a path that does not exist: no such entry, or a component
		// that is a file (ENOTDIR on POSIX; Windows reports the path as not found). Both mean "no entry at this path", so
		// a path below a file behaves the same on every host.
		static bool IsMissingPathError(const std::error_code& error)
		{
			return error == std::errc::no_such_file_or_directory || error == std::errc::not_a_directory;
		}

		static ErrorCode ErrorCodeFromSystem(const std::error_code& error)
		{
			if (error == std::errc::no_such_file_or_directory)
				return ErrorCode::NotFound;
			if (error == std::errc::permission_denied || error == std::errc::operation_not_permitted)
				return ErrorCode::PermissionDenied;
			if (error == std::errc::file_exists)
				return ErrorCode::AlreadyExists;
			return ErrorCode::Io;
		}

		// The OS reason for an error message. Some C runtimes return it in a legacy code page; error messages must stay
		// valid UTF-8 (they reach JSON-RPC responses), so such text is replaced by the numeric code.
		static std::string ReasonOf(const std::error_code& error)
		{
			std::string reason = error.message();
			while (!reason.empty() && (reason.back() == '\n' || reason.back() == '\r' || reason.back() == ' '))
				reason.pop_back();
			if (reason.empty() || !IsValidUtf8(reason))
				return std::format("OS error {}", error.value());
			return reason;
		}

		static std::unexpected<Error> SystemError(const std::error_code& error, std::string_view action, const std::filesystem::path& path)
		{
			return MakeError(ErrorCodeFromSystem(error), "cannot {} '{}': {}", action, PathToUtf8(path), ReasonOf(error));
		}

		// The error of a failed standard stream operation. The streams report no reason themselves; every supported C
		// runtime sets errno in the underlying open, read or write, so it is used when set.
		static std::unexpected<Error> StreamError(int errorNumber, std::string_view action, const std::filesystem::path& path)
		{
			if (errorNumber == 0)
				return MakeError(ErrorCode::Io, "cannot {} '{}'", action, PathToUtf8(path));
			return SystemError(std::error_code(errorNumber, std::generic_category()), action, path);
		}

		static std::string_view AtomicWriteStepToString(AtomicWriteStep step)
		{
			switch (step)
			{
				case AtomicWriteStep::None:            return "None";
				case AtomicWriteStep::CreateTemporary: return "CreateTemporary";
				case AtomicWriteStep::Write:           return "Write";
				case AtomicWriteStep::Flush:           return "Flush";
				case AtomicWriteStep::Backup:          return "Backup";
				case AtomicWriteStep::Replace:         return "Replace";
			}

			ENGINE_CORE_ASSERT(false, "Unknown AtomicWriteStep {}", std::to_underlying(step));
			return "Unknown";
		}

		// SplitMix64's finalizer: spreads the bits of the inputs of a temporary file name.
		static uint64_t MixBits(uint64_t value)
		{
			value ^= value >> 30;
			value *= 0xbf58476d1ce4e5b9ull;
			value ^= value >> 27;
			value *= 0x94d049bb133111ebull;
			return value ^ (value >> 31);
		}

		// A name for the temporary file of an atomic write that no concurrent writer picks: the clocks separate
		// sequential writes and processes, the thread separates concurrent writers in this process, and `attempt` separates
		// retries after a name was found taken.
		static std::string TemporarySuffix(int attempt)
		{
			const auto steady = static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
			const auto wall = static_cast<uint64_t>(std::chrono::system_clock::now().time_since_epoch().count());
			const auto thread = static_cast<uint64_t>(std::hash<std::thread::id>()(std::this_thread::get_id()));
			const uint64_t unique = MixBits(steady ^ MixBits(wall ^ MixBits(thread + static_cast<uint64_t>(attempt))));
			return std::format(".tmp-{:016x}", unique);
		}

		static bool IsLexicallyUnder(const std::filesystem::path& path, const std::filesystem::path& directory)
		{
			return IsStrictlyUnder(PathToUtf8(path.lexically_normal()), PathToUtf8(directory.lexically_normal()));
		}

	}

	Result<Buffer> FileSystem::ReadFile(const std::filesystem::path& path)
	{
		std::error_code error;
		const std::filesystem::file_status status = std::filesystem::status(path, error);
		if (error && !Utils::IsMissingPathError(error))
			return Utils::SystemError(error, "read", path);
		if (!std::filesystem::exists(status))
			return MakeError(ErrorCode::NotFound, "cannot read '{}': no such file", Utils::PathToUtf8(path));
		if (std::filesystem::is_directory(status))
			return MakeError(ErrorCode::Io, "cannot read '{}': it is a directory", Utils::PathToUtf8(path));

		errno = 0;
		std::ifstream stream(path, std::ios::binary);
		if (!stream.is_open())
			return Utils::StreamError(errno, "open", path);

		// The size is only a hint for the first request (one byte more, so a file that did not change reaches its end in
		// one read): the loop reads to the end whatever the file holds by then.
		const uintmax_t sizeHint = std::filesystem::file_size(path, error);
		size_t request = Utils::ReadChunkSize;
		if (!error && sizeHint < std::numeric_limits<size_t>::max())
			request = static_cast<size_t>(sizeHint) + 1;

		Buffer buffer;
		while (true)
		{
			const size_t offset = buffer.size();
			buffer.resize(offset + request);
			errno = 0;
			stream.read(reinterpret_cast<char*>(buffer.data() + offset), static_cast<std::streamsize>(request));
			const auto count = static_cast<size_t>(stream.gcount());
			buffer.resize(offset + count);
			if (stream.bad())
				return Utils::StreamError(errno, "read", path);
			if (stream.eof())
				break;
			if (stream.fail())
				return Utils::StreamError(errno, "read", path);
			request = std::max(Utils::ReadChunkSize, buffer.size());
		}
		return buffer;
	}

	Result<std::string> FileSystem::ReadText(const std::filesystem::path& path)
	{
		ENGINE_TRY_ASSIGN(const Buffer bytes, ReadFile(path));
		const std::string_view text = AsStringView(bytes);
		const size_t invalidOffset = FindInvalidUtf8(text);
		if (invalidOffset != text.size())
		{
			return MakeError(ErrorCode::Validation, "'{}' is not valid UTF-8: invalid byte sequence at offset {}", Utils::PathToUtf8(path),
				invalidOffset);
		}
		return std::string(text);
	}

	Status FileSystem::WriteFileAtomic(const std::filesystem::path& path, std::span<const std::byte> data, const AtomicWriteOptions& options)
	{
		const std::filesystem::path fileName = path.filename();
		if (fileName.empty() || fileName == "." || fileName == "..")
			return MakeError(ErrorCode::InvalidArgument, "cannot write '{}': the path names no file", Utils::PathToUtf8(path));

		std::error_code error;
		const std::filesystem::path parent = path.parent_path();
		const std::filesystem::path directory = parent.empty() ? std::filesystem::path(".") : parent;
		const std::filesystem::file_status directoryStatus = std::filesystem::status(directory, error);
		if (error && !Utils::IsMissingPathError(error))
			return Utils::SystemError(error, "write into", directory);
		if (!std::filesystem::is_directory(directoryStatus))
		{
			return MakeError(ErrorCode::NotFound, "cannot write '{}': the directory '{}' does not exist", Utils::PathToUtf8(path),
				Utils::PathToUtf8(directory));
		}

		const std::filesystem::file_status targetStatus = std::filesystem::status(path, error);
		if (error && !Utils::IsMissingPathError(error))
			return Utils::SystemError(error, "write", path);
		if (std::filesystem::is_directory(targetStatus))
			return MakeError(ErrorCode::Io, "cannot write '{}': it is a directory", Utils::PathToUtf8(path));
		const bool targetExists = std::filesystem::exists(targetStatus);

		const auto stepError = [&path](AtomicWriteStep step, Error cause)
		{
			return std::move(cause).WithContext(
				std::format("while writing '{}' atomically (step {})", Utils::PathToUtf8(path), Utils::AtomicWriteStepToString(step)));
		};
		const auto injected = [&path](AtomicWriteStep step)
		{
			return Error(ErrorCode::Io,
				std::format("injected failure at step {} of the atomic write of '{}'", Utils::AtomicWriteStepToString(step),
					Utils::PathToUtf8(path)));
		};

		// Step CreateTemporary.
		if (options.InjectFailure == AtomicWriteStep::CreateTemporary)
			return std::unexpected(injected(AtomicWriteStep::CreateTemporary));

		std::filesystem::path temporary;
		for (int attempt = 0; attempt < Utils::TemporaryNameAttempts && temporary.empty(); ++attempt)
		{
			std::filesystem::path candidate = path;
			candidate += Utils::PathFromUtf8(Utils::TemporarySuffix(attempt));
			const bool taken = std::filesystem::exists(candidate, error);
			if (!error && !taken)
				temporary = std::move(candidate);
		}
		if (temporary.empty())
		{
			return std::unexpected(stepError(AtomicWriteStep::CreateTemporary,
				Error(ErrorCode::Io, std::format("no free temporary file name next to '{}'", Utils::PathToUtf8(path)))));
		}

		errno = 0;
		std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
		if (!stream.is_open())
			return std::unexpected(stepError(AtomicWriteStep::CreateTemporary, Utils::StreamError(errno, "create", temporary).error()));

		// Every later failure closes and removes the temporary, so the directory is left as it was.
		const auto abandon = [&stream, &temporary, &stepError](AtomicWriteStep step, Error cause) -> Status
		{
			if (stream.is_open())
				stream.close();
			Error failure = stepError(step, std::move(cause));
			std::error_code removeError;
			std::filesystem::remove(temporary, removeError);
			if (!removeError)
				return std::unexpected(std::move(failure));
			return std::unexpected(std::move(failure).WithContext(
				std::format("and the temporary file '{}' could not be removed: {}", Utils::PathToUtf8(temporary), Utils::ReasonOf(removeError))));
		};

		// Step Write.
		if (options.InjectFailure == AtomicWriteStep::Write)
			return abandon(AtomicWriteStep::Write, injected(AtomicWriteStep::Write));
		errno = 0;
		if (!data.empty())
			stream.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
		if (!stream)
			return abandon(AtomicWriteStep::Write, Utils::StreamError(errno, "write", temporary).error());

		// Step Flush.
		if (options.InjectFailure == AtomicWriteStep::Flush)
			return abandon(AtomicWriteStep::Flush, injected(AtomicWriteStep::Flush));
		errno = 0;
		stream.flush();
		if (!stream)
			return abandon(AtomicWriteStep::Flush, Utils::StreamError(errno, "flush", temporary).error());
		stream.close();
		if (stream.fail())
			return abandon(AtomicWriteStep::Flush, Utils::StreamError(errno, "close", temporary).error());

		// Step Backup: a copy, so the target itself stays in place until the atomic rename below.
		if (options.KeepBackup && targetExists)
		{
			if (options.InjectFailure == AtomicWriteStep::Backup)
				return abandon(AtomicWriteStep::Backup, injected(AtomicWriteStep::Backup));
			std::filesystem::path backup = path;
			backup += ".bak";
			std::filesystem::copy_file(path, backup, std::filesystem::copy_options::overwrite_existing, error);
			if (error)
				return abandon(AtomicWriteStep::Backup, Utils::SystemError(error, "back up the file to", backup).error());
		}

		// Step Replace.
		if (options.InjectFailure == AtomicWriteStep::Replace)
			return abandon(AtomicWriteStep::Replace, injected(AtomicWriteStep::Replace));
		std::filesystem::rename(temporary, path, error);
		if (error)
			return abandon(AtomicWriteStep::Replace, Utils::SystemError(error, "replace", path).error());
		return {};
	}

	bool FileSystem::Exists(const std::filesystem::path& path)
	{
		std::error_code error;
		const bool exists = std::filesystem::exists(path, error);
		return !error && exists;
	}

	Result<FileInfo> FileSystem::GetInfo(const std::filesystem::path& path)
	{
		std::error_code error;
		const std::filesystem::file_status status = std::filesystem::status(path, error);
		if (error && !Utils::IsMissingPathError(error))
			return Utils::SystemError(error, "query", path);
		if (!std::filesystem::exists(status))
			return MakeError(ErrorCode::NotFound, "cannot query '{}': no such file or directory", Utils::PathToUtf8(path));

		const std::filesystem::file_time_type modified = std::filesystem::last_write_time(path, error);
		if (error)
			return Utils::SystemError(error, "query the modification time of", path);

		FileInfo info;
		info.ModificationTime = static_cast<uint64_t>(modified.time_since_epoch().count());
		info.IsDirectory = std::filesystem::is_directory(status);
		if (!info.IsDirectory)
		{
			const uintmax_t size = std::filesystem::file_size(path, error);
			if (error)
				return Utils::SystemError(error, "query the size of", path);
			info.Size = static_cast<uint64_t>(size);
		}
		return info;
	}

	Status FileSystem::CreateDirectories(const std::filesystem::path& path)
	{
		std::error_code error;
		const std::filesystem::file_status status = std::filesystem::status(path, error);
		if (error && !Utils::IsMissingPathError(error))
			return Utils::SystemError(error, "create the directory", path);
		if (std::filesystem::is_directory(status))
			return {};
		if (std::filesystem::exists(status))
			return MakeError(ErrorCode::AlreadyExists, "cannot create the directory '{}': a file is in the way", Utils::PathToUtf8(path));

		std::filesystem::create_directories(path, error);
		if (!error)
			return {};

		// A file where a parent directory should be is reported differently by each host (ENOTDIR, ERROR_PATH_NOT_FOUND).
		for (std::filesystem::path ancestor = path.parent_path(); !ancestor.empty(); ancestor = ancestor.parent_path())
		{
			std::error_code ancestorError;
			const std::filesystem::file_status ancestorStatus = std::filesystem::status(ancestor, ancestorError);
			if (!ancestorError && std::filesystem::exists(ancestorStatus) && !std::filesystem::is_directory(ancestorStatus))
			{
				return MakeError(ErrorCode::AlreadyExists, "cannot create the directory '{}': the file '{}' is in the way",
					Utils::PathToUtf8(path), Utils::PathToUtf8(ancestor));
			}
			if (ancestor == ancestor.parent_path())
				break;
		}
		return Utils::SystemError(error, "create the directory", path);
	}

	Status FileSystem::Remove(const std::filesystem::path& path)
	{
		std::error_code error;
		const std::filesystem::file_status status = std::filesystem::symlink_status(path, error);
		if (error && !Utils::IsMissingPathError(error))
			return Utils::SystemError(error, "remove", path);
		if (!std::filesystem::exists(status))
			return MakeError(ErrorCode::NotFound, "cannot remove '{}': no such file or directory", Utils::PathToUtf8(path));

		std::filesystem::remove_all(path, error);
		if (error)
			return Utils::SystemError(error, "remove", path);
		return {};
	}

	Status FileSystem::Move(const std::filesystem::path& from, const std::filesystem::path& to)
	{
		std::error_code error;
		const std::filesystem::file_status fromStatus = std::filesystem::symlink_status(from, error);
		if (error && !Utils::IsMissingPathError(error))
			return Utils::SystemError(error, "move", from);
		if (!std::filesystem::exists(fromStatus))
			return MakeError(ErrorCode::NotFound, "cannot move '{}': no such file or directory", Utils::PathToUtf8(from));
		if (Utils::IsLexicallyUnder(to, from))
		{
			return MakeError(ErrorCode::InvalidArgument, "cannot move '{}' into itself ('{}')", Utils::PathToUtf8(from),
				Utils::PathToUtf8(to));
		}

		const std::filesystem::file_status toStatus = std::filesystem::symlink_status(to, error);
		if (error && !Utils::IsMissingPathError(error))
			return Utils::SystemError(error, "move to", to);
		if (std::filesystem::exists(toStatus))
		{
			// On a case-insensitive host a case-only rename finds its own source at the destination.
			const std::string fromText = Utils::PathToUtf8(from);
			const std::string toText = Utils::PathToUtf8(to);
			const bool sameEntry = std::filesystem::equivalent(from, to, error);
			const bool isCaseOnlyRename = !error && sameEntry && fromText != toText && Utils::EqualsIgnoreAsciiCase(fromText, toText);
			if (!isCaseOnlyRename)
				return MakeError(ErrorCode::AlreadyExists, "cannot move '{}' to '{}': the destination exists", fromText, toText);
		}

		std::filesystem::rename(from, to, error);
		if (error)
			return Utils::SystemError(error, std::format("move '{}' to", Utils::PathToUtf8(from)), to);
		return {};
	}

	Result<std::vector<std::filesystem::path>> FileSystem::ListDirectory(const std::filesystem::path& directory, bool recursive)
	{
		std::error_code error;
		const std::filesystem::file_status status = std::filesystem::status(directory, error);
		if (error && !Utils::IsMissingPathError(error))
			return Utils::SystemError(error, "list", directory);
		if (!std::filesystem::exists(status))
			return MakeError(ErrorCode::NotFound, "cannot list '{}': no such directory", Utils::PathToUtf8(directory));
		if (!std::filesystem::is_directory(status))
			return MakeError(ErrorCode::Io, "cannot list '{}': it is not a directory", Utils::PathToUtf8(directory));

		std::vector<std::pair<std::string, std::filesystem::path>> entries;
		if (recursive)
		{
			std::filesystem::recursive_directory_iterator iterator(directory, std::filesystem::directory_options::none, error);
			if (error)
				return Utils::SystemError(error, "list", directory);
			const std::filesystem::recursive_directory_iterator end;
			while (iterator != end)
			{
				entries.emplace_back(Utils::PathToUtf8(iterator->path()), iterator->path());
				iterator.increment(error);
				if (error)
					return Utils::SystemError(error, "list", directory);
			}
		}
		else
		{
			std::filesystem::directory_iterator iterator(directory, error);
			if (error)
				return Utils::SystemError(error, "list", directory);
			const std::filesystem::directory_iterator end;
			while (iterator != end)
			{
				entries.emplace_back(Utils::PathToUtf8(iterator->path()), iterator->path());
				iterator.increment(error);
				if (error)
					return Utils::SystemError(error, "list", directory);
			}
		}

		std::ranges::sort(entries, {}, &std::pair<std::string, std::filesystem::path>::first);
		std::vector<std::filesystem::path> paths;
		paths.reserve(entries.size());
		for (std::pair<std::string, std::filesystem::path>& entry : entries)
			paths.push_back(std::move(entry.second));
		return paths;
	}

	Status FileSystem::VerifyCase(const std::filesystem::path& root, std::string_view relativePath)
	{
		const size_t invalidOffset = FindInvalidUtf8(relativePath);
		if (invalidOffset != relativePath.size())
			return MakeError(ErrorCode::InvalidArgument, "cannot check the case of a path with invalid UTF-8 (byte offset {})", invalidOffset);

		std::error_code error;
		const std::filesystem::file_status rootStatus = std::filesystem::status(root, error);
		if (error && !Utils::IsMissingPathError(error))
			return Utils::SystemError(error, "list", root);
		if (!std::filesystem::is_directory(rootStatus))
			return MakeError(ErrorCode::NotFound, "the directory '{}' does not exist", Utils::PathToUtf8(root));
		if (relativePath.empty())
			return {};

		std::filesystem::path current = root;
		std::string onDisk;
		size_t start = 0;
		while (true)
		{
			const size_t end = relativePath.find('/', start);
			const bool isLast = end == std::string_view::npos;
			const std::string_view component = relativePath.substr(start, isLast ? std::string_view::npos : end - start);
			const std::string_view given = relativePath.substr(0, isLast ? relativePath.size() : end);
			if (component.empty() || component == "." || component == "..")
				return MakeError(ErrorCode::InvalidArgument, "cannot check the case of '{}': empty or relative component", relativePath);

			std::filesystem::directory_iterator iterator(current, error);
			if (error)
				return Utils::SystemError(error, "list", current);
			const std::filesystem::directory_iterator iteratorEnd;
			std::filesystem::path match;
			std::string variant;
			while (iterator != iteratorEnd)
			{
				const std::string name = Utils::FileNameToUtf8(iterator->path());
				if (name == component)
				{
					match = iterator->path();
					break;
				}
				if (variant.empty() && Utils::EqualsIgnoreAsciiCase(name, component))
					variant = name;
				iterator.increment(error);
				if (error)
					return Utils::SystemError(error, "list", current);
			}

			if (match.empty())
			{
				if (!variant.empty())
					return MakeError(ErrorCode::Validation, "case mismatch: '{}' is '{}' on disk", given, Utils::JoinRelative(onDisk, variant));
				return MakeError(ErrorCode::NotFound, "'{}' does not exist in '{}'", given, Utils::PathToUtf8(root));
			}

			onDisk = Utils::JoinRelative(onDisk, component);
			if (isLast)
				return {};

			const bool isDirectory = std::filesystem::is_directory(match, error);
			if (error && !Utils::IsMissingPathError(error))
				return Utils::SystemError(error, "query", match);
			if (!isDirectory)
				return MakeError(ErrorCode::NotFound, "'{}' does not exist in '{}': '{}' is not a directory", relativePath, Utils::PathToUtf8(root), onDisk);
			current = std::move(match);
			start = end + 1;
		}
	}

}
