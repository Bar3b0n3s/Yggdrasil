#include "EditorPCH.h"
#include "Editor/Panels/ContentBrowserPanel.h"

#include "Editor/EditorPanelContext.h"
#include "Editor/Icons.h"
#include "EditorCore/EditorActions.h"
#include "EditorCore/EditorContext.h"
#include "EditorCore/Thumbnails/ThumbnailCache.h"
#include "Engine/Asset/AssetRegistry.h"
#include "Engine/AssetPipeline/EditorAssetManager.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/VirtualFileSystem.h"

#include <imgui.h>

#include <algorithm>
#include <cstring>
#include <functional>

namespace Engine {

	Status ContentBrowserPanel::Draw(EditorPanelContext& context)
	{
		auto& editor = context.Editor;
		if (!editor.HasProject())
		{
			ImGui::TextUnformatted("Open a project to browse assets.");
			return {};
		}
		const std::string project = FileSystem::PathToUtf8(editor.GetProject().GetProjectFile());
		if (m_Project != project)
		{
			m_Project = project;
			m_Directory = "Assets";
			m_Search.fill(0);
			m_Error.clear();
		}
		const auto showError = [this](const Error& error)
		{
			m_Error = error.ToString();
			ENGINE_ERROR("Content browser: {}", m_Error);
		};
		std::erase_if(m_Tickets, [&context, &showError](uint64_t ticket)
		{
			auto completion = context.Actions.TakeResult(ticket);
			if (!completion)
			{
				showError(completion.error());
				return true;
			}
			if (!*completion)
				return false;
			if (!**completion)
				showError((**completion).error());
			return true;
		});
		const auto submit = [this, &context, &showError](std::string_view method, const Json& params)
		{
			auto ticket = context.Actions.Submit(method, params);
			if (!ticket)
				showError(ticket.error());
			else
			{
				m_Tickets.push_back(*ticket);
				m_Error.clear();
			}
		};
		if (context.TakeContentDrops)
		{
			for (const auto& source : context.TakeContentDrops())
				submit("asset.import", Json{ { "source", FileSystem::PathToUtf8(source) }, { "destDir", m_Directory } });
		}
		ImGui::InputText("Search assets", m_Search.data(), m_Search.size());
		if (ImGui::Button("Refresh assets"))
			submit("project.refreshAssets", Json::object());
		ImGui::SameLine();
		ImGui::TextUnformatted(m_Directory.c_str());
		ImGui::BeginDisabled(editor.IsReadOnly());
		if (ImGui::CollapsingHeader("Create or import"))
		{
			constexpr const char* Types[] = { "Folder", "Scene", "Material", "Prefab", "SoundEffect" };
			constexpr const char* Extensions[] = { "", ".scene", ".material", ".prefab", ".sfx" };
			ImGui::Combo("Type", &m_CreateType, Types, static_cast<int>(std::size(Types)));
			ImGui::InputText("Name", m_CreateName.data(), m_CreateName.size());
			if (ImGui::Button("Create"))
			{
				std::string name = m_CreateName.data();
				if (!name.empty() && name.find_first_of("/\\:") == std::string::npos && name != "." && name != "..")
				{
					if (!name.ends_with(Extensions[m_CreateType]))
						name += Extensions[m_CreateType];
					Json params{ { "type", Types[m_CreateType] }, { "path", m_Directory + "/" + name } };
					if (std::string_view(Types[m_CreateType]) == "SoundEffect")
						params["values"] = Json{ { "Layers", Json::array({ Json{ { "Duration", 0.1 } } }) } };
					submit("asset.create", params);
				}
				else
					showError(Error(ErrorCode::InvalidArgument, "enter a non-empty filename, without directory separators"));
			}
			ImGui::TextDisabled("Script templates become available in M13.");
			ImGui::InputText("Import source path", m_ImportSource.data(), m_ImportSource.size());
			if (ImGui::Button("Import"))
				submit("asset.import", Json{ { "source", m_ImportSource.data() }, { "destDir", m_Directory } });
		}
		ImGui::EndDisabled();

		if (ImGui::BeginTable("ContentColumns", 2, ImGuiTableFlags_Resizable))
		{
			ImGui::TableSetupColumn("Folders", ImGuiTableColumnFlags_WidthFixed, 170.0f);
			ImGui::TableSetupColumn("Assets", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableNextColumn();
			const auto root = VfsPath::Create("project", "Assets");
			if (!root)
			{
				ImGui::EndTable();
				return std::unexpected(root.error());
			}
			const auto folders = editor.GetVfs().List(*root, true);
			if (!folders)
			{
				ImGui::TextWrapped("%s", folders.error().ToString().c_str());
			}
			else
			{
				std::function<void(std::string_view)> drawFolder;
				drawFolder = [this, &folders, &drawFolder, &editor, &submit](std::string_view path)
				{
					const std::string owned(path);
					const auto slash = path.rfind('/');
					const std::string label(path.substr(slash == std::string_view::npos ? 0 : slash + 1));
					ImGui::PushID(owned.c_str());
					const bool expanded = ImGui::TreeNodeEx("##Folder", ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_DefaultOpen | (m_Directory == path ? ImGuiTreeNodeFlags_Selected : 0), "%s", label.c_str());
					if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
						m_Directory = path;
					if (!editor.IsReadOnly() && ImGui::BeginDragDropTarget())
					{
						if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ENGINE_ASSET"); payload && payload->DataSize == 17)
						{
							const char* text = static_cast<const char*>(payload->Data);
							const auto handle = text[16] == '\0' ? UUID::FromString(std::string_view(text, 16)) : std::nullopt;
							if (handle && handle->IsValid() && handle->ToString() == std::string_view(text, 16))
							{
								const auto location = editor.GetAssets().GetRegistry().Locate(*handle);
								if (location && location->SubAssetKey.empty())
									submit("asset.move", Json{ { "asset", handle->ToString() }, { "path", owned + "/" + std::string(location->Record->SourcePath.GetFileName()) } });
							}
						}
						ImGui::EndDragDropTarget();
					}
					if (expanded)
					{
						for (const auto& entry : *folders)
							if (entry.Info.IsDirectory && entry.Path.GetParent().GetPath() == path)
								drawFolder(entry.Path.GetPath());
						ImGui::TreePop();
					}
					ImGui::PopID();
				};
				drawFolder("Assets");
			}
			ImGui::TableNextColumn();
			std::vector<AssetHandle> handles = editor.GetAssets().GetRegistry().GetHandles();
			std::sort(handles.begin(), handles.end(), [&editor](AssetHandle left, AssetHandle right)
			{
				const auto leftPath = editor.GetAssets().GetReferencePath(left);
				const auto rightPath = editor.GetAssets().GetReferencePath(right);
				return leftPath == rightPath ? left < right : leftPath < rightPath;
			});
			const int columns = std::max(1, static_cast<int>(ImGui::GetContentRegionAvail().x / 120.0f));
			if (ImGui::BeginTable("AssetGrid", columns))
			{
				for (const AssetHandle handle : handles)
				{
					const auto location = editor.GetAssets().GetRegistry().Locate(handle);
					if (!location || location->Record->SourcePath.GetParent().GetPath() != m_Directory)
						continue;
					const std::string path = editor.GetAssets().GetReferencePath(handle);
					if (!path.contains(m_Search.data()))
						continue;
					ImGui::TableNextColumn();
					const std::string id = handle.ToString();
					ImGui::PushID(id.c_str());
					const ImVec2 minimum = ImGui::GetCursorScreenPos();
					if (ImGui::Selectable("##Asset", editor.GetUiState().GetSelectedAsset() == handle, ImGuiSelectableFlags_AllowDoubleClick, ImVec2(100.0f, 82.0f)))
					{
						editor.SetSelection({});
						editor.GetUiState().SetSelectedAsset(handle);
						m_MovePath.fill(0);
						std::copy_n(path.begin(), std::min(path.size(), m_MovePath.size() - 1), m_MovePath.begin());
						if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && location->Type == AssetType::Scene)
							submit("scene.open", Json{ { "path", path } });
					}
					if (ImGui::BeginDragDropSource())
					{
						ImGui::SetDragDropPayload("ENGINE_ASSET", id.c_str(), id.size() + 1);
						ImGui::TextUnformatted(path.c_str());
						ImGui::EndDragDropSource();
					}
					uint64_t texture = 0;
					const ThumbnailRequest request{ context.Thumbnails.GetProjectGeneration(), handle, editor.GetAssets().GetVersion(handle), 128 };
					if (request.ProjectGeneration != 0)
					{
						const auto cached = context.Thumbnails.Find(request);
						if (cached && !*cached)
						{
							if (const Status queued = context.Thumbnails.Queue(request); !queued)
								ImGui::TextWrapped("%s", queued.error().GetMessageText().c_str());
						}
						else if (!cached)
							ImGui::TextWrapped("%s", cached.error().GetMessageText().c_str());
						else if (!(*cached)->TypeIcon && context.FindThumbnailTexture)
							texture = context.FindThumbnailTexture(request);
					}
					if (texture != 0)
						ImGui::GetWindowDrawList()->AddImage(ImTextureRef(texture), ImVec2(minimum.x + 18.0f, minimum.y + 8.0f), ImVec2(minimum.x + 82.0f, minimum.y + 72.0f));
					else
						DrawEditorIcon(*ImGui::GetWindowDrawList(), AssetTypeToEditorIcon(location->Type), glm::vec2(minimum.x + 24.0f, minimum.y + 8.0f), 48.0f, ImGui::GetColorU32(ImGuiCol_Text));
					const std::string label = location->SubAssetKey.empty() ? std::string(location->Record->SourcePath.GetFileName()) : location->SubAssetKey;
					ImGui::TextWrapped("%s", label.c_str());
					for (const auto& diagnostic : editor.GetAssets().GetDiagnostics())
						if (diagnostic.Path == location->Record->SourcePath.GetPath())
						{
							ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.2f, 1.0f), "Import issue");
							if (ImGui::IsItemHovered())
								ImGui::SetTooltip("%s", diagnostic.Message.c_str());
							break;
						}
					ImGui::PopID();
				}
				ImGui::EndTable();
			}
			ImGui::EndTable();
		}
		const AssetHandle selected = editor.GetUiState().GetSelectedAsset();
		const auto location = editor.GetAssets().GetRegistry().Locate(selected);
		if (location)
		{
			ImGui::SeparatorText("Selected asset");
			ImGui::TextUnformatted(editor.GetAssets().GetReferencePath(selected).c_str());
			ImGui::BeginDisabled(editor.IsReadOnly() || !location->SubAssetKey.empty());
			if (ImGui::Button("Reimport"))
				submit("asset.reimport", Json{ { "asset", selected.ToString() } });
			ImGui::SameLine();
			if (ImGui::Button("Move to trash"))
				submit("asset.delete", Json{ { "asset", selected.ToString() } });
			ImGui::InputText("New asset path", m_MovePath.data(), m_MovePath.size());
			if (ImGui::Button("Rename / move"))
				submit("asset.move", Json{ { "asset", selected.ToString() }, { "path", m_MovePath.data() } });
			ImGui::EndDisabled();
			if (location->Type == AssetType::AudioClip)
			{
				ImGui::BeginDisabled(!context.QueueAudioPreview);
				if (ImGui::Button("Preview audio"))
					if (const Status queued = context.QueueAudioPreview(selected); !queued)
						showError(queued.error());
				ImGui::SameLine();
				if (ImGui::Button("Stop audio"))
					if (const Status queued = context.QueueAudioPreview({}); !queued)
						showError(queued.error());
				ImGui::EndDisabled();
				if (!context.QueueAudioPreview)
					ImGui::TextDisabled("Audio preview service is unavailable.");
			}
		}
		if (!m_Tickets.empty())
		{
			ImGui::Text("%zu pending asset operations", m_Tickets.size());
			if (ImGui::Button("Cancel pending operations"))
				for (const uint64_t ticket : m_Tickets)
					if (const Status cancelled = context.Actions.Cancel(ticket); !cancelled)
						showError(cancelled.error());
		}
		if (!m_Error.empty())
			ImGui::TextWrapped("%s", m_Error.c_str());
		return {};
	}

}
