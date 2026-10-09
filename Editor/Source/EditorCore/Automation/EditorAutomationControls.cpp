#include "EditorPCH.h"
#include "EditorCore/Automation/EditorAutomationControls.h"

#include "EditorCore/Automation/AutomationServer.h"
#include "EditorCore/EditorContext.h"
#include "EditorCore/EditorPreferences.h"
#include "Engine/Core/VirtualFileSystem.h"

namespace Engine {

	EditorAutomationControls::EditorAutomationControls(EditorContext& context, AutomationServer& server)
		: m_Context(&context), m_Server(&server)
	{
	}

	EditorAutomationControls::~EditorAutomationControls() = default;

	EditorAutomationPolicy EditorAutomationControls::GetPolicy() const
	{
		return m_Server->GetEditorPolicy();
	}

	void EditorAutomationControls::SetPolicy(const EditorAutomationPolicy& policy)
	{
		m_Server->SetEditorPolicy(policy);
	}

	std::vector<EditorRequestActivity> EditorAutomationControls::GetRecentRequests() const
	{
		return m_Server->GetRecentRequestActivity();
	}

	Status EditorAutomationControls::SetAllowAiAutomation(bool allowed)
	{
		VirtualFileSystem& vfs = m_Context->GetVfs();
		ENGINE_TRY_ASSIGN(EditorPreferences preferences, ReadEditorPreferences(vfs));
		ENGINE_TRY_ASSIGN(const VfsPath path, VfsPath::Parse("user://Editor.json"));
		Result<Buffer> previous = vfs.ReadFile(path);
		if (!previous && previous.error().GetCode() != ErrorCode::NotFound)
			return std::unexpected(std::move(previous).error());
		preferences.AllowAiAutomation = allowed;
		ENGINE_TRY(WriteEditorPreferences(vfs, preferences));
		Status listening = m_Server->SetPreferenceListening(allowed);
		if (listening)
			return {};
		// Restore the exact document, including its absence and unknown fields, rather than serializing defaults.
		const Status restored = previous ? vfs.WriteFileAtomic(path, *previous) : vfs.Remove(path);
		if (!restored)
			return MakeError(ErrorCode::Io, "listener change failed ({}); restoring preferences also failed ({})", listening.error(), restored.error());
		return std::unexpected(std::move(listening).error().WithContext("changing the AI automation listener; previous preferences restored"));
	}

}
