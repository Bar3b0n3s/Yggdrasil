#include "EnginePCH.h"
#include "Engine/Automation/Protocol/SessionFile.h"

#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/Json/JsonWriter.h"

#include <nlohmann/json.hpp>

#include <format>
#include <limits>
#include <system_error>

// The token is the server's only secret, so the session directory is 0700 and the file 0600 on POSIX hosts
// (std::filesystem::permissions; on Windows it only toggles the read-only attribute and the per-user %LOCALAPPDATA% ACL
// does the work). The file is written to a temporary name, restricted, and then renamed over the final name, so no
// reader ever sees it with wider permissions or half written.

namespace Engine {

	namespace Utils {

		// A civil (proleptic Gregorian) date.
		struct CivilDate
		{
			int64_t Year = 1970;
			uint32_t Month = 1;
			uint32_t Day = 1;
		};

		// The date `days` days after 1970-01-01 (negative: before it), by Howard Hinnant's civil_from_days algorithm, which
		// is exact for every day count a 64-bit clock can produce.
		[[nodiscard]] static CivilDate CivilFromDays(int64_t days)
		{
			days += 719468;
			const int64_t era = (days >= 0 ? days : days - 146096) / 146097;
			const auto dayOfEra = static_cast<uint64_t>(days - era * 146097);                                     // [0, 146096]
			const uint64_t yearOfEra = (dayOfEra - dayOfEra / 1460 + dayOfEra / 36524 - dayOfEra / 146096) / 365; // [0, 399]
			const uint64_t dayOfYear = dayOfEra - (365 * yearOfEra + yearOfEra / 4 - yearOfEra / 100);            // [0, 365]
			const uint64_t monthIndex = (5 * dayOfYear + 2) / 153;                                                // [0, 11]
			CivilDate date;
			date.Day = static_cast<uint32_t>(dayOfYear - (153 * monthIndex + 2) / 5 + 1);
			date.Month = static_cast<uint32_t>(monthIndex < 10 ? monthIndex + 3 : monthIndex - 9);
			date.Year = static_cast<int64_t>(yearOfEra) + era * 400 + (date.Month <= 2 ? 1 : 0);
			return date;
		}

		// The canonical text of `content`. Errors: Validation for a member that is not valid UTF-8.
		[[nodiscard]] static Result<std::string> WriteSessionText(const SessionFileContent& content)
		{
			JsonWriter writer(JsonStyle::Pretty);
			writer.BeginObject();
			writer.WriteKey("pid");
			writer.WriteUInt(content.Pid);
			writer.WriteKey("port");
			writer.WriteUInt(content.Port);
			writer.WriteKey("token");
			writer.WriteString(content.Token);
			writer.WriteKey("protocolVersion");
			writer.WriteString(content.ProtocolVersionText);
			writer.WriteKey("engineVersion");
			writer.WriteString(content.EngineVersionText);
			writer.WriteKey("projectPath");
			writer.WriteString(content.ProjectPath);
			writer.WriteKey("headless");
			writer.WriteBool(content.Headless);
			writer.WriteKey("startedAt");
			writer.WriteString(content.StartedAt);
			writer.EndObject();
			return writer.Finish();
		}

		// An Io error about the session file of `pid`.
		[[nodiscard]] static std::unexpected<Error> MakeSessionFileError(std::string_view action, uint32_t pid, const std::error_code& error)
		{
			return MakeError(ErrorCode::Io, "cannot {} the session file of process {}: {}", action, pid, error.message());
		}

	}

	std::filesystem::path SessionFile::GetPath(const std::filesystem::path& directory, uint32_t pid)
	{
		return directory / std::format("{}.json", pid);
	}

	std::string SessionFile::ToText(const SessionFileContent& content)
	{
		// The members are engine-generated ASCII except the project path, which a broken file system could make invalid
		// UTF-8; the file then names no project rather than a wrong one.
		Result<std::string> text = Utils::WriteSessionText(content);
		if (text)
			return std::move(*text);
		SessionFileContent withoutProject = content;
		withoutProject.ProjectPath.clear();
		return Utils::WriteSessionText(withoutProject).value_or(std::string());
	}

	Result<SessionFileContent> SessionFile::FromText(std::string_view text)
	{
		ENGINE_TRY_ASSIGN(const Json document, JsonReader::Parse(text));
		const JsonReader reader(document);
		ENGINE_TRY(reader.ExpectType(JsonType::Object));

		SessionFileContent content;
		ENGINE_TRY_ASSIGN(content.Pid, reader.ReadMember<uint32_t>("pid"));
		ENGINE_TRY_ASSIGN(const JsonReader port, reader.GetMember("port"));
		ENGINE_TRY_ASSIGN(const uint32_t portNumber, port.ReadUInt32());
		if (portNumber > std::numeric_limits<uint16_t>::max())
			return std::unexpected(port.MakeLocatedError(ErrorCode::Validation, std::format("port {} is not a TCP port", portNumber)));
		content.Port = static_cast<uint16_t>(portNumber);
		ENGINE_TRY_ASSIGN(content.Token, reader.ReadMember<std::string>("token"));
		ENGINE_TRY_ASSIGN(content.ProtocolVersionText, reader.ReadMember<std::string>("protocolVersion"));
		ENGINE_TRY_ASSIGN(content.EngineVersionText, reader.ReadMember<std::string>("engineVersion"));
		ENGINE_TRY_ASSIGN(content.ProjectPath, reader.ReadMember<std::string>("projectPath"));
		ENGINE_TRY_ASSIGN(content.Headless, reader.ReadMember<bool>("headless"));
		ENGINE_TRY_ASSIGN(content.StartedAt, reader.ReadMember<std::string>("startedAt"));
		return content;
	}

	Status SessionFile::Write(const std::filesystem::path& directory, const SessionFileContent& content)
	{
		ENGINE_TRY(FileSystem::CreateDirectories(directory));
		std::error_code error;
		std::filesystem::permissions(directory, std::filesystem::perms::owner_all, std::filesystem::perm_options::replace, error);
		if (error)
			return Utils::MakeSessionFileError("restrict the directory of", content.Pid, error);

		const std::filesystem::path file = GetPath(directory, content.Pid);
		std::filesystem::path temporary = file;
		temporary += ".tmp";
		const std::string text = ToText(content);
		ENGINE_TRY(FileSystem::WriteFileAtomic(temporary, std::as_bytes(std::span(text.data(), text.size())), { .KeepBackup = false }));

		std::filesystem::permissions(temporary, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
			std::filesystem::perm_options::replace, error);
		if (!error)
			std::filesystem::rename(temporary, file, error);
		if (error)
		{
			std::error_code removeError;
			std::filesystem::remove(temporary, removeError);
			return Utils::MakeSessionFileError("write", content.Pid, error);
		}
		return {};
	}

	Status SessionFile::Remove(const std::filesystem::path& directory, uint32_t pid)
	{
		const Status removed = FileSystem::Remove(GetPath(directory, pid));
		if (!removed && removed.error().GetCode() != ErrorCode::NotFound)
			return removed;
		return {};
	}

	std::string SessionFile::FormatUtcTimestamp(std::chrono::system_clock::time_point time, TimestampPrecision precision)
	{
		const int64_t milliseconds = std::chrono::floor<std::chrono::milliseconds>(time.time_since_epoch()).count();
		constexpr int64_t MillisecondsPerDay = 86400000;
		const int64_t days = (milliseconds >= 0 ? milliseconds : milliseconds - (MillisecondsPerDay - 1)) / MillisecondsPerDay;
		const int64_t millisecondOfDay = milliseconds - days * MillisecondsPerDay;
		const int64_t secondOfDay = millisecondOfDay / 1000;
		const Utils::CivilDate date = Utils::CivilFromDays(days);
		const std::string text = std::format("{:04}-{:02}-{:02}T{:02}:{:02}:{:02}", date.Year, date.Month, date.Day, secondOfDay / 3600,
			(secondOfDay % 3600) / 60, secondOfDay % 60);
		if (precision == TimestampPrecision::Milliseconds)
			return std::format("{}.{:03}Z", text, millisecondOfDay % 1000);
		return text + "Z";
	}

}
