#include "EditorPCH.h"
#include "EditorCore/Automation/TranscriptLog.h"

#include "Engine/Automation/Protocol/SessionFile.h"
#include "Engine/Core/Buffer.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/Json/JsonWriter.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/VirtualFileSystem.h"

#include <nlohmann/json.hpp>

#include <chrono>

namespace Engine {

	namespace Utils {

		constexpr std::string_view TranscriptDirectory = "Automation";

		// The current UTC time as ISO 8601 with milliseconds ("2026-10-06T17:48:24.512Z"). The transcript records when a call
		// happened, so wall-clock time is the right clock here (it never feeds simulation).
		static std::string FormatUtcNow()
		{
			return SessionFile::FormatUtcTimestamp(std::chrono::system_clock::now(), TimestampPrecision::Milliseconds);
		}

		// The transcript's current bytes (empty when the file does not exist yet), ending with a complete line: a torn last line
		// (a writer that died mid-line) is completed with "\n" and keeps its number, as the bridge completes it
		// (Tools/MCP/engine_mcp/transcript.py), so every line number keeps its meaning and nothing is edited by hand
		// (Docs/Decisions/0026-m16-contract.md decision 13; the audit reports the torn line). The bytes are read and counted
		// as bytes, never decoded, as the bridge does: a line torn inside a multi-byte UTF-8 character is not valid UTF-8, and
		// it must not stop every later append.
		static Result<std::string> ReadTranscript(const VirtualFileSystem& vfs, const VfsPath& path)
		{
			Result<Buffer> bytes = vfs.ReadFile(path);
			if (!bytes)
			{
				if (bytes.error().GetCode() == ErrorCode::NotFound)
					return std::string();
				return std::unexpected(std::move(bytes).error().WithContext(std::format("while reading '{}'", path.ToString())));
			}
			std::string text(AsStringView(*bytes));
			if (!text.empty() && text.back() != '\n')
			{
				ENGINE_WARN("'{}' ended with a torn line; it was completed with a newline and keeps its line number", path.ToString());
				text += '\n';
			}
			return text;
		}

		// Appends `line` (one JSON object, written minified, plus "\n") to the transcript by an atomic rewrite, because the
		// VFS has no append (ADR 0008 decision 11). Returns the line's 1-based number.
		static Result<uint64_t> AppendLine(VirtualFileSystem& vfs, const Json& line)
		{
			ENGINE_TRY_ASSIGN(const VfsPath directory, VfsPath::Create("project", TranscriptDirectory));
			ENGINE_TRY_ASSIGN(const VfsPath path, VfsPath::Create("project", TranscriptLog::FilePath));
			ENGINE_TRY_ASSIGN(std::string text, ReadTranscript(vfs, path));
			ENGINE_TRY_ASSIGN(const std::string encoded, JsonWriter::Write(line, JsonStyle::Minified));

			const auto lineNumber = static_cast<uint64_t>(std::count(text.begin(), text.end(), '\n')) + 1;
			text += encoded;
			text += '\n';
			ENGINE_TRY(vfs.CreateDirectories(directory));
			ENGINE_TRY(vfs.WriteFileAtomic(path, std::as_bytes(std::span(text.data(), text.size()))));
			return lineNumber;
		}

	}

	Result<uint64_t> TranscriptLog::AppendRequest(VirtualFileSystem& vfs, std::string_view client, const Json& id, std::string_view method,
		const Json& params)
	{
		Json line = Json::object();
		line["type"] = "request";
		line["time"] = Utils::FormatUtcNow();
		line["client"] = std::string(client);
		line["id"] = id;
		line["method"] = std::string(method);
		line["params"] = params;
		return Utils::AppendLine(vfs, line);
	}

	Status TranscriptLog::AppendResponse(VirtualFileSystem& vfs, std::string_view client, const Json& id, uint64_t requestLine,
		std::string_view summary, const Json& error)
	{
		Json line = Json::object();
		line["type"] = "response";
		line["time"] = Utils::FormatUtcNow();
		line["client"] = std::string(client);
		line["id"] = id;
		line["requestLine"] = requestLine;
		line["ok"] = error.is_null();
		line["summary"] = std::string(summary);
		if (!error.is_null())
		{
			const JsonReader reader(error);
			Json copied = Json::object();
			if (const std::optional<JsonReader> code = reader.FindMember("code"))
				copied["code"] = code->GetValue();
			if (const std::optional<JsonReader> data = reader.FindMember("data"))
			{
				if (const std::optional<JsonReader> errorCode = data->FindMember("errorCode"))
					copied["errorCode"] = errorCode->GetValue();
				if (const std::optional<JsonReader> detail = data->FindMember("detail"))
					copied["detail"] = detail->GetValue();
			}
			line["error"] = std::move(copied);
		}
		ENGINE_TRY(Utils::AppendLine(vfs, line));
		return {};
	}

}
