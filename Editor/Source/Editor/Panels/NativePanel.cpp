#include "EditorPCH.h"
#include "Editor/Panels/NativePanel.h"

#include "Editor/EditorPanelContext.h"
#include "Editor/Ui/EditorStyle.h"
#include "EditorCore/EditorActions.h"
#include "EditorCore/EditorContext.h"
#include "EditorCore/Native/NativeModuleHost.h"
#include "EditorCore/Play/EditorPlayController.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/Json/JsonWriter.h"
#include "Engine/Native/NativeComponents.h"
#include "Engine/Native/NativeModule.h"
#include "Engine/Platform/SharedLibrary.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Session/PlaySession.h"

#include <imgui.h>

#include <format>
#include <span>
#include <utility>

namespace Engine {

	namespace Utils {

		// The JSON of a native component as the panel shows it.
		[[nodiscard]] static std::string FormatNativeValue(const Json& value)
		{
			Result<std::string> text = JsonWriter::Write(value, JsonStyle::Minified);
			return text.has_value() ? std::move(*text) : std::string("(a value that cannot be shown)");
		}

		[[nodiscard]] static std::string DescribeNativeDiagnostic(const NativeBuildDiagnostic& diagnostic)
		{
			std::string location = diagnostic.File;
			if (!location.empty() && diagnostic.Line != 0)
				location += std::format(":{}", diagnostic.Line);
			return location.empty() ? std::format("{}: {}", diagnostic.Code, diagnostic.Message) : std::format("{}: {}: {}", location, diagnostic.Code, diagnostic.Message);
		}

		// A boolean member of an action's result, false when it is absent or of another type.
		[[nodiscard]] static bool ReadResultFlag(const Json& result, std::string_view member)
		{
			const std::optional<JsonReader> value = JsonReader(result).FindMember(member);
			return value.has_value() && value->ReadBool().value_or(false);
		}

		static void DrawNativeTypeList(std::string_view heading, std::span<const NativeTypeInfo> types)
		{
			if (types.empty())
				return;
			ImGui::TextDisabled("%.*s", static_cast<int>(heading.size()), heading.data());
			for (const NativeTypeInfo& type : types)
			{
				ImGui::BulletText("%s", type.RegistryName.c_str());
				ImGui::SetItemTooltip("%s", type.Description.c_str());
			}
		}

	}

	void NativePanel::RefreshStatus(EditorPanelContext& context)
	{
		EditorContext& editor = context.Editor;
		m_StatusProject = editor.GetProject().GetProjectFile();
		const Ref<const NativeModuleTypes> loaded = editor.GetNativeModule().GetLoadedTypes();
		m_StatusModule = loaded;
		m_StatusHadModule = loaded != nullptr;
		StatusSnapshot snapshot;
		Result<NativeModuleStatus> status = editor.GetNativeModule().GetStatus(editor);
		if (!status.has_value())
		{
			snapshot.Error = status.error().ToString();
			m_Status = std::move(snapshot);
			return;
		}
		snapshot.Sources = std::move(status->Sources);
		snapshot.SourceHash = std::move(status->SourceHash);
		if (status->LatestBuild.has_value())
		{
			snapshot.LatestBuild = FileSystem::PathToUtf8(status->LatestBuild->Directory.filename());
			snapshot.LatestBuildCurrent = status->LatestBuild->Current;
		}
		snapshot.Building = status->Building;
		if (status->LastBuild.has_value())
		{
			snapshot.LastBuildSucceeded = status->LastBuild->Succeeded;
			for (const NativeBuildDiagnostic& diagnostic : status->LastBuild->Diagnostics)
				snapshot.LastBuildDiagnostics.push_back(Utils::DescribeNativeDiagnostic(diagnostic));
		}
		snapshot.LoadedCurrent = status->Loaded != nullptr && !snapshot.SourceHash.empty() && status->LoadedSourceHash == snapshot.SourceHash;
		m_Status = std::move(snapshot);
	}

	void NativePanel::Submit(EditorPanelContext& context, std::string_view method, std::optional<uint64_t>& ticket)
	{
		Result<uint64_t> submitted = context.Actions.Submit(method, Json::object());
		if (!submitted.has_value())
		{
			m_ActionMessage = submitted.error().GetMessageText();
			m_ActionFailed = true;
			return;
		}
		ticket = *submitted;
		m_ActionMessage.clear();
		m_ActionFailed = false;
	}

	bool NativePanel::TakeResults(EditorPanelContext& context)
	{
		bool resolved = false;
		for (std::optional<uint64_t>* ticket : { &m_BuildTicket, &m_LoadTicket })
		{
			if (!ticket->has_value())
				continue;
			const bool build = ticket == &m_BuildTicket;
			Result<std::optional<Result<Json>>> result = context.Actions.TakeResult(**ticket);
			if (result.has_value() && !result->has_value())
				continue;
			ticket->reset();
			resolved = true;
			if (!result.has_value() || !(*result)->has_value())
			{
				const Error& error = !result.has_value() ? result.error() : (*result)->error();
				m_ActionMessage = std::format("{} failed: {}", build ? "The build" : "Loading", error.GetMessageText());
				m_ActionFailed = true;
				continue;
			}
			const Json& value = ***result;
			m_ActionFailed = build && !Utils::ReadResultFlag(value, "succeeded");
			if (build)
				m_ActionMessage = m_ActionFailed ? "The build failed; its diagnostics are listed below." : "Built the module.";
			else
			{
				m_ActionMessage = Utils::ReadResultFlag(value, "reloaded") ? "Reloaded the module." : "Loaded the module.";
				if (Utils::ReadResultFlag(value, "sceneChanged"))
					m_ActionMessage += " The native components of the open scenes were migrated; save the scenes to keep them.";
			}
		}
		return resolved;
	}

	void NativePanel::DrawPreference(EditorPanelContext& context)
	{
		EditorContext& editor = context.Editor;
		NativeModuleHost& host = editor.GetNativeModule();
		const bool allowed = host.IsAllowed();
		// The host applies a queued change at the next frame boundary (EditorContext::Update, before the frame's UI); one that
		// a whole frame later still differs could not be saved, and the host logged why.
		const uint64_t frame = editor.GetUiState().GetCompletedFrame();
		if (m_RequestedAllowed.has_value() && (*m_RequestedAllowed == allowed || frame > m_RequestedFrame))
		{
			m_PreferenceFailed = *m_RequestedAllowed != allowed;
			m_RequestedAllowed.reset();
		}
		bool shown = m_RequestedAllowed.value_or(allowed);
		if (ImGui::Checkbox("Allow native code", &shown))
		{
			host.QueuePreferenceChange(shown);
			m_RequestedAllowed = shown;
			m_RequestedFrame = frame;
			m_PreferenceFailed = false;
		}
		ImGui::SetItemTooltip("Lets this editor build the project's Native/ C++ sources, load them into the editor and link them into exported games. "
							  "Connected agents can write those sources, so this gives them code execution on this computer. Only you can change it.");
		if (m_RequestedAllowed.has_value())
			ImGui::TextDisabled("Saving the preference");
		if (m_PreferenceFailed)
			ImGui::TextWrapped("The preference could not be saved, so it stays as it was; the Console names the problem.");
		ImGui::TextWrapped("%s", allowed ? "Native code is allowed for every project this editor opens. This preference is saved for future sessions." : "Native code is off: the native module is neither built nor loaded, and a game with one does not export.");
	}

	void NativePanel::DrawModule(EditorPanelContext& context, const NativeModuleTypes* loaded)
	{
		EditorContext& editor = context.Editor;
		NativeModuleHost& host = editor.GetNativeModule();
		Utils::EditorSectionHeading("Module");
		if (!SharedLibrary::CheckSupported().has_value())
		{
			ImGui::TextWrapped("Native modules are built, loaded and exported on Windows only.");
			return;
		}
		if (!m_Status.has_value())
			return;
		const StatusSnapshot& status = *m_Status;
		if (!status.Error.empty())
		{
			ImGui::TextWrapped("%s", status.Error.c_str());
			return;
		}
		if (status.Sources.empty())
		{
			Utils::EditorEmptyState("No native module", "A game's native module is the .cpp and .h files below its Native/ folder; agents write them with native.write, "
														"one of them naming its registration function with ENGINE_NATIVE_MODULE.");
			return;
		}
		ImGui::TextDisabled("Sources");
		for (const std::string& source : status.Sources)
			ImGui::BulletText("%s", source.c_str());
		if (status.LatestBuild.has_value())
			ImGui::TextWrapped("Newest build: %s%s", status.LatestBuild->c_str(), status.LatestBuildCurrent ? "" : " (the sources changed since)");
		else
			ImGui::TextWrapped("Not built yet.");

		const bool pending = m_BuildTicket.has_value() || m_LoadTicket.has_value();
		const bool playing = editor.GetPlay().IsPlaying();
		ImGui::BeginDisabled(!host.IsAllowed() || editor.IsReadOnly() || status.Building || pending);
		if (ImGui::Button("Build"))
			Submit(context, "native.build", m_BuildTicket);
		ImGui::EndDisabled();
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
			ImGui::SetTooltip("Build the module for this editor (native.build).");
		ImGui::SameLine();
		ImGui::BeginDisabled(!host.IsAllowed() || !status.LatestBuildCurrent || playing || status.Building || pending);
		if (ImGui::Button(loaded != nullptr ? "Reload" : "Load"))
			Submit(context, "native.load", m_LoadTicket);
		ImGui::EndDisabled();
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
			ImGui::SetTooltip("Load the newest build of the sources as they are now, migrating the open scene's native components (native.load). "
							  "Loading waits for Edit mode.");
		ImGui::SameLine();
		if (ImGui::Button("Refresh"))
			RefreshStatus(context);
		if (m_BuildTicket.has_value() || status.Building)
			ImGui::TextDisabled("Building");
		if (!m_ActionMessage.empty())
		{
			if (m_ActionFailed)
				ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.48f, 0.48f, 1.0f));
			ImGui::TextWrapped("%s", m_ActionMessage.c_str());
			if (m_ActionFailed)
				ImGui::PopStyleColor();
		}
		if (status.LastBuildSucceeded.has_value())
		{
			ImGui::TextDisabled("%s", *status.LastBuildSucceeded ? "Last build succeeded" : "Last build failed");
			for (const std::string& diagnostic : status.LastBuildDiagnostics)
				ImGui::TextWrapped("%s", diagnostic.c_str());
		}

		Utils::EditorSectionHeading("Loaded module");
		if (loaded == nullptr)
		{
			ImGui::TextWrapped("No module is loaded.");
			return;
		}
		if (!status.LoadedCurrent)
			ImGui::TextWrapped("The loaded module was built from sources that changed since: build and reload it.");
		Utils::DrawNativeTypeList("Components", loaded->GetComponents());
		Utils::DrawNativeTypeList("Resources", loaded->GetResources());
		if (!loaded->GetSystems().empty())
		{
			ImGui::TextDisabled("Systems, in run order");
			for (const NativeSystemInfo& system : loaded->GetSystems())
			{
				ImGui::BulletText("%s", system.Name.c_str());
				ImGui::SetItemTooltip("%s", system.Description.c_str());
			}
		}
	}

	void NativePanel::DrawSelection(EditorPanelContext& context, const NativeModuleTypes* loaded)
	{
		EditorContext& editor = context.Editor;
		const std::span<const UUID> selection = editor.GetSelection();
		const PlaySession* session = editor.GetPlay().GetSession();
		const Scene* scene = editor.GetSelectionTarget() == SceneTarget::Play && session != nullptr ? &session->GetScene()
			: editor.HasScene()                                                                     ? &editor.GetScene()
																									: nullptr;
		if (selection.empty() || scene == nullptr)
			return;
		const ConstEntity entity = scene->FindEntityByID(selection.front());
		if (!entity.IsValid())
			return;
		const NativeEntityComponents components = ReadNativeComponents(entity, loaded);
		Utils::EditorSectionHeading("Selected entity");
		if (components.Registered.empty() && components.Unregistered.empty())
		{
			ImGui::TextWrapped("%s has no native components.", entity.GetName().c_str());
			return;
		}
		for (const auto& [name, value] : components.Registered)
			ImGui::TextWrapped("%s %s", name.c_str(), Utils::FormatNativeValue(value.Get()).c_str());
		for (const auto& [name, value] : components.Unregistered)
			ImGui::TextWrapped("%s (not registered by the loaded module) %s", name.c_str(), Utils::FormatNativeValue(value.Get()).c_str());
		ImGui::TextDisabled("Read-only here: agents change native components with native.setComponents.");
	}

	Status NativePanel::Draw(EditorPanelContext& context)
	{
		EditorContext& editor = context.Editor;
		DrawPreference(context);
		if (!editor.HasProject())
		{
			m_Status.reset();
			Utils::EditorEmptyState("No project open", "Open a project to build and load its native module.");
			return {};
		}
		// The module held for this frame only, so a reload's previous library unloads once the host lets it go.
		const Ref<const NativeModuleTypes> loaded = editor.GetNativeModule().GetLoadedTypes();
		const bool resolved = TakeResults(context);
		// An expired observation never equals a module loaded since, whatever its address.
		const bool moduleChanged = m_StatusHadModule != (loaded != nullptr) || m_StatusModule.lock() != loaded;
		if (!m_Status.has_value() || resolved || m_StatusProject != editor.GetProject().GetProjectFile() || moduleChanged)
			RefreshStatus(context);
		DrawModule(context, loaded.get());
		DrawSelection(context, loaded.get());
		return {};
	}

}
