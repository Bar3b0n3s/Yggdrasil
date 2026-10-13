#include "EditorPCH.h"
#include "Editor/Private/EditorHostPreferences.h"

#include "EditorCore/Project/ProjectManager.h"
#include "Engine/Core/Utf8.h"

#include <algorithm>
#include <string_view>
#include <utility>

namespace Engine {

	Status EditorHostPreferences::Load(const VirtualFileSystem& vfs)
	{
		Result<EditorPreferences> preferences = ReadEditorPreferences(vfs);
		if (!preferences.has_value())
		{
			m_Preferences = {};
			m_Automation = { .Allowed = false, .Pending = false, .Failure = preferences.error() };
			return std::unexpected(std::move(preferences).error().WithContext("reading user://Editor.json; the defaults are used"));
		}
		m_Preferences = std::move(*preferences);
		m_Automation = { .Allowed = m_Preferences.AllowAiAutomation, .Pending = false, .Failure = std::nullopt };
		return {};
	}

	Status EditorHostPreferences::StartListening(const Apply& listen)
	{
		if (const Status started = listen(m_Automation.Allowed); !started.has_value())
		{
			m_Automation.Allowed = false;
			m_Automation.Failure = started.error();
			return std::unexpected(Error(started.error()).WithContext("starting the AI automation listener; it stays off"));
		}
		return {};
	}

	Status EditorHostPreferences::QueueAutomationChange(bool allowed)
	{
		if (m_Pending.has_value())
			return MakeError(ErrorCode::InvalidState, "A preference change is already queued");
		m_Pending = allowed;
		m_Automation.Pending = true;
		m_Automation.Failure.reset();
		return {};
	}

	Status EditorHostPreferences::Pump(const Apply& apply)
	{
		if (!m_Pending.has_value())
			return {};
		const bool allowed = *std::exchange(m_Pending, std::nullopt);
		const Status applied = apply(allowed);
		m_Automation.Pending = false;
		if (!applied.has_value())
		{
			m_Automation.Failure = applied.error();
			return applied;
		}
		m_Automation.Allowed = allowed;
		m_Automation.Failure.reset();
		m_Preferences.AllowAiAutomation = allowed;
		return {};
	}

	EditorExternalEditorState EditorHostPreferences::GetExternalEditorState() const
	{
		return {
			.Executable = m_Preferences.ExternalEditorExecutable,
			.Arguments = m_Preferences.ExternalEditorArguments,
			.Pending = m_PendingExternalEditor.has_value(),
			.Failure = m_ExternalEditorFailure,
		};
	}

	Status EditorHostPreferences::QueueExternalEditorChange(std::string executable, std::vector<std::string> arguments)
	{
		if (m_PendingExternalEditor.has_value())
			return MakeError(ErrorCode::InvalidState, "An external editor change is already queued");
		const auto valid = [](std::string_view text)
		{
			return IsValidUtf8(text) && text.find('\0') == std::string_view::npos;
		};
		if (!valid(executable) || !std::ranges::all_of(arguments, valid))
			return MakeError(ErrorCode::InvalidArgument, "The external editor and its arguments must be UTF-8 text without NUL characters");
		if (executable.empty() && !arguments.empty())
			return MakeError(ErrorCode::InvalidArgument, "Choose the external editor's executable before its arguments");
		const auto namesPath = [](const std::string& argument)
		{
			return argument.contains("{path}");
		};
		if (!arguments.empty() && std::ranges::none_of(arguments, namesPath))
			return MakeError(ErrorCode::InvalidArgument, "One argument must contain {{path}}, which becomes the script's path");
		m_PendingExternalEditor = ExternalEditorChange{ .Executable = std::move(executable), .Arguments = std::move(arguments) };
		m_ExternalEditorFailure.reset();
		return {};
	}

	Status EditorHostPreferences::PumpExternalEditor(VirtualFileSystem& vfs)
	{
		if (!m_PendingExternalEditor.has_value())
			return {};
		ExternalEditorChange change = *std::exchange(m_PendingExternalEditor, std::nullopt);
		Result<EditorPreferences> stored = ReadEditorPreferences(vfs);
		if (!stored.has_value())
		{
			m_ExternalEditorFailure = stored.error();
			return std::unexpected(std::move(stored).error().WithContext("reading user://Editor.json"));
		}
		stored->ExternalEditorExecutable = std::move(change.Executable);
		stored->ExternalEditorArguments = std::move(change.Arguments);
		if (const Status written = WriteEditorPreferences(vfs, *stored); !written.has_value())
		{
			m_ExternalEditorFailure = written.error();
			return std::unexpected(Error(written.error()).WithContext("writing user://Editor.json"));
		}
		m_Preferences.ExternalEditorExecutable = std::move(stored->ExternalEditorExecutable);
		m_Preferences.ExternalEditorArguments = std::move(stored->ExternalEditorArguments);
		return {};
	}

	void EditorHostPreferences::RefreshRecentProjects(const VirtualFileSystem& vfs)
	{
		m_RecentProjects = ProjectManager::ReadRecentProjects(vfs);
	}

}
