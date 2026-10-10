#include "EditorPCH.h"
#include "Editor/Panels/ContentBrowserPanel.h"

#include "Editor/EditorPanelContext.h"
#include "Editor/Icons.h"
#include "Editor/Ui/EditorStyle.h"
#include "EditorCore/EditorActions.h"
#include "EditorCore/EditorContext.h"
#include "EditorCore/Thumbnails/ThumbnailCache.h"
#include "Engine/Asset/AssetRegistry.h"
#include "Engine/AssetPipeline/EditorAssetManager.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Hash.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/VirtualFileSystem.h"

#include <imgui.h>

#include <algorithm>
#include <functional>

namespace Engine {

	namespace Utils {

		static std::string AssetSearchKey(std::string text)
		{
			for (char& character : text)
				if (character >= 'A' && character <= 'Z')
					character = static_cast<char>(character + ('a' - 'A'));
			return text;
		}

		static std::string AssetTileLabel(std::string text, float width)
		{
			if (ImGui::CalcTextSize(text.c_str()).x <= width)
				return text;
			while (!text.empty())
			{
				size_t end = text.size() - 1;
				while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xc0) == 0x80)
					--end;
				text.resize(end);
				if (ImGui::CalcTextSize((text + "...").c_str()).x <= width)
					return text + "...";
			}
			return "...";
		}

		static bool AssetNameValid(std::string_view name)
		{
			return !name.empty() && name != "." && name != ".." && name.find_first_of("/\\:") == std::string_view::npos;
		}

	}

	Status ContentBrowserPanel::Draw(EditorPanelContext& context)
	{
		auto& editor = context.Editor;
		const auto showError = [this](const Error& error)
		{
			m_Error = error.GetMessageText();
			ENGINE_ERROR("Content browser: {}", error.ToString());
		};
		const std::string project = editor.HasProject() ? FileSystem::PathToUtf8(editor.GetProject().GetProjectFile()) : std::string{};
		if (m_Project != project)
		{
			for (const uint64_t ticket : m_Tickets)
				if (const Status cancelled = context.Actions.Cancel(ticket); !cancelled)
					showError(cancelled.error());
			m_Project = project;
			m_Directory = "Assets";
			m_Search.fill(0);
			m_CreateName.fill(0);
			m_ImportSource.fill(0);
			m_ImportPicker.Cancel();
			m_AssetDialog = 0;
			m_ActionAsset = {};
			m_ReopenImport = false;
			m_Error.clear();
		}
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
			if (!**completion && (**completion).error().GetCode() != ErrorCode::Cancelled)
				showError((**completion).error());
			return true;
		});
		if (!editor.HasProject())
		{
			Utils::EditorEmptyState("Your assets live here", "Open or create a project to browse scenes, materials, models and audio.");
			return {};
		}
		// Project identity scopes transient popups as well as their input state.
		ImGui::PushID(static_cast<int>(FNV1a32(m_Project)));
		const auto submit = [this, &context, &showError](std::string_view method, const Json& params) -> bool
		{
			auto ticket = context.Actions.Submit(method, params);
			if (!ticket)
			{
				showError(ticket.error());
				return false;
			}
			m_Tickets.push_back(*ticket);
			m_Error.clear();
			return true;
		};
		if (context.TakeContentDrops)
		{
			for (const auto& source : context.TakeContentDrops())
				if (!editor.IsReadOnly())
					submit("asset.import", Json{ { "source", FileSystem::PathToUtf8(source) }, { "destDir", m_Directory } });
		}
		const float scale = Utils::EditorUiScale();
		ImGui::BeginDisabled(editor.IsReadOnly());
		if (Utils::EditorToolbarButton("Create", "Create an asset in the current folder"))
		{
			m_ActionDirectory = m_Directory;
			m_CreateName.fill(0);
			ImGui::OpenPopup("Create asset");
		}
		ImGui::SameLine();
		if (Utils::EditorToolbarButton("Import", "Browse for a file to add to this project"))
		{
			m_ImportDirectory = m_Directory;
			ImGui::OpenPopup("Import asset");
		}
		ImGui::EndDisabled();
		ImGui::SameLine();
		if (Utils::EditorToolbarButton("Refresh", "Scan project assets for changes"))
			submit("project.refreshAssets", Json::object());
		if (ImGui::GetContentRegionAvail().x > 200.0f * scale)
			ImGui::SameLine();
		ImGui::SetNextItemWidth(-1.0f);
		ImGui::InputTextWithHint("##SearchAssets", "Search this folder...", m_Search.data(), m_Search.size());

		std::string breadcrumb;
		const std::string directory = m_Directory;
		for (size_t start = 0; start < directory.size();)
		{
			const size_t end = directory.find('/', start);
			const std::string part = directory.substr(start, end == std::string::npos ? end : end - start);
			breadcrumb += (breadcrumb.empty() ? "" : "/") + part;
			const std::string label = Utils::AssetTileLabel(part, std::max(40.0f * scale, ImGui::GetContentRegionAvail().x - 30.0f * scale));
			ImGui::PushID(static_cast<int>(FNV1a32(breadcrumb)));
			const float padding = ImGui::GetStyle().FramePadding.x;
			ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(padding, 0.0f));
			if (ImGui::Button("##Breadcrumb", ImVec2(ImGui::CalcTextSize(label.c_str()).x + padding * 2.0f, ImGui::GetTextLineHeight())))
				m_Directory = breadcrumb;
			ImGui::PopStyleVar();
			const ImVec2 minimum = ImGui::GetItemRectMin();
			ImGui::GetWindowDrawList()->AddText(ImVec2(minimum.x + padding, minimum.y), ImGui::GetColorU32(ImGuiCol_Text), label.c_str());
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("%s", breadcrumb.c_str());
			ImGui::PopID();
			if (end == std::string::npos)
				break;
			if (ImGui::GetContentRegionAvail().x > ImGui::CalcTextSize(part.c_str()).x + 100.0f * scale)
				ImGui::SameLine();
			start = end + 1;
		}

		const auto root = VfsPath::Create("project", "Assets");
		if (!root)
		{
			ImGui::PopID();
			return std::unexpected(root.error());
		}
		const auto folders = editor.GetVfs().List(*root, true);
		const auto moveDrop = [&editor, &submit](std::string_view target)
		{
			if (!editor.IsReadOnly() && ImGui::BeginDragDropTarget())
			{
				if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ENGINE_ASSET"); payload && payload->DataSize == 17)
				{
					const char* text = static_cast<const char*>(payload->Data);
					const auto handle = text[16] == '\0' ? UUID::FromString(std::string_view(text, 16)) : std::nullopt;
					if (handle && handle->IsValid() && handle->ToString() == std::string_view(text, 16))
					{
						const auto location = editor.GetAssets().GetRegistry().Locate(*handle);
						if (location && location->SubAssetKey.empty() && location->Record->SourcePath.GetParent().GetPath() != target)
							submit("asset.move", Json{ { "asset", handle->ToString() }, { "path", std::string(target) + "/" + std::string(location->Record->SourcePath.GetFileName()) } });
					}
				}
				ImGui::EndDragDropTarget();
			}
		};
		const auto requestDialog = [this, &editor](AssetHandle handle, int dialog)
		{
			const auto location = editor.GetAssets().GetRegistry().Locate(handle);
			if (!location)
				return;
			m_ActionAsset = handle;
			m_AssetDialog = dialog;
			const std::string path = dialog == 1 ? std::string(location->Record->SourcePath.GetFileName()) : std::string(location->Record->SourcePath.GetParent().GetPath());
			m_MovePath.fill(0);
			std::copy_n(path.begin(), std::min(path.size(), m_MovePath.size() - 1), m_MovePath.begin());
		};
		size_t visibleAssets = 0;
		const float footer = ImGui::GetFrameHeightWithSpacing() + (!m_Error.empty() ? 2.0f * ImGui::GetTextLineHeightWithSpacing() : 0.0f)
			+ (!m_Tickets.empty() ? ImGui::GetFrameHeightWithSpacing() : 0.0f);
		const float minimumRow = 2.0f * ImGui::GetTextLineHeightWithSpacing() + 8.0f * scale
			+ 2.0f * ImGui::GetStyle().CellPadding.y + ImGui::GetStyle().ItemSpacing.y;
		const float bodyHeight = std::max(minimumRow, ImGui::GetContentRegionAvail().y - footer);
		const bool showTree = ImGui::GetContentRegionAvail().x > 440.0f * scale;
		if (ImGui::BeginTable("ContentColumns", showTree ? 2 : 1, ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp))
		{
			if (showTree)
				ImGui::TableSetupColumn("Folders", ImGuiTableColumnFlags_WidthFixed, 170.0f * scale);
			ImGui::TableSetupColumn("Assets", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableNextColumn();
			if (showTree)
			{
				if (ImGui::BeginChild("FolderTree", ImVec2(0.0f, bodyHeight), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar))
				{
					ImGui::TextDisabled("Folders");
					if (!folders)
						ImGui::TextWrapped("%s", folders.error().GetMessageText().c_str());
					else
					{
						std::function<void(std::string_view)> drawFolder;
						drawFolder = [this, &folders, &drawFolder, &moveDrop](std::string_view path)
						{
							const std::string owned(path);
							const auto slash = path.rfind('/');
							const std::string label(path.substr(slash == std::string_view::npos ? 0 : slash + 1));
							const bool hasChildren = std::any_of(folders->begin(), folders->end(), [path](const auto& entry)
							{
								return entry.Info.IsDirectory && entry.Path.GetParent().GetPath() == path;
							});
							ImGui::PushID(static_cast<int>(FNV1a32(owned)));
							ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
							if (path == "Assets")
								flags |= ImGuiTreeNodeFlags_DefaultOpen;
							if (!hasChildren)
								flags |= ImGuiTreeNodeFlags_Leaf;
							if (m_Directory == path)
								flags |= ImGuiTreeNodeFlags_Selected;
							const ImVec2 minimum = ImGui::GetCursorScreenPos();
							const bool expanded = ImGui::TreeNodeEx("##Folder", flags, "%s", "");
							ImDrawList* draw = ImGui::GetWindowDrawList();
							draw->PushClipRect(minimum, ImGui::GetItemRectMax(), true);
							draw->AddText(ImVec2(minimum.x + ImGui::GetTreeNodeToLabelSpacing(), minimum.y), ImGui::GetColorU32(ImGuiCol_Text), label.c_str());
							draw->PopClipRect();
							if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
								m_Directory = path;
							moveDrop(path);
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
				}
				ImGui::EndChild();
				ImGui::TableNextColumn();
			}
			if (ImGui::BeginChild("AssetTiles", ImVec2(0.0f, bodyHeight)))
			{
				const std::string search = Utils::AssetSearchKey(m_Search.data());
				const int columns = std::max(1, static_cast<int>(ImGui::GetContentRegionAvail().x / (136.0f * scale)));
				// Bottom docks have little vertical space: shrink the preview before sacrificing either text line.
				const float textHeight = 2.0f * ImGui::GetTextLineHeightWithSpacing();
				const float rowSpace = bodyHeight - ImGui::GetStyle().CellPadding.y * 2.0f - ImGui::GetStyle().ItemSpacing.y;
				const bool compact = rowSpace < textHeight + 36.0f * scale;
				const float previewSize = compact ? 28.0f * scale : std::clamp(rowSpace - textHeight - 12.0f * scale, 24.0f * scale, 56.0f * scale);
				const float labelOffset = compact ? 4.0f * scale : previewSize + 8.0f * scale;
				const float tileHeight = labelOffset + textHeight + 4.0f * scale;
				std::optional<std::string> nextDirectory;
				size_t visibleFolders = 0;
				if (ImGui::BeginTable("AssetGrid", columns, ImGuiTableFlags_SizingStretchSame))
				{
					const auto drawTile = [tileHeight, scale, compact, previewSize, labelOffset](std::string_view name, std::string_view type, EditorIcon icon, uint64_t texture, ImVec2 minimum, bool selected, bool hovered, bool issue)
					{
						ImDrawList* draw = ImGui::GetWindowDrawList();
						const float width = ImGui::GetItemRectSize().x;
						const ImVec2 maximum(minimum.x + width, minimum.y + tileHeight);
						draw->AddRectFilled(minimum, maximum, ImGui::GetColorU32(selected ? ImGuiCol_Header : hovered ? ImGuiCol_FrameBgHovered
																													  : ImGuiCol_FrameBg),
							ImGui::GetStyle().FrameRounding);
						if (selected)
							draw->AddRect(minimum, maximum, ImGui::GetColorU32(ImGuiCol_CheckMark), ImGui::GetStyle().FrameRounding);
						const float imageSize = std::max(1.0f, std::min(previewSize, width - 12.0f * scale));
						const ImVec2 imageMin(minimum.x + (compact ? 6.0f * scale : (width - imageSize) * 0.5f), minimum.y + (compact ? (tileHeight - imageSize) * 0.5f : 4.0f * scale));
						if (texture)
							draw->AddImage(ImTextureRef(texture), imageMin, ImVec2(imageMin.x + imageSize, imageMin.y + imageSize));
						else
							DrawEditorIcon(*draw, icon, glm::vec2(imageMin.x, imageMin.y), imageSize, ImGui::GetColorU32(selected ? ImGuiCol_Text : ImGuiCol_TextDisabled));
						const float inset = compact ? previewSize + 12.0f * scale : 6.0f * scale;
						const float textWidth = std::max(1.0f, width - inset - 6.0f * scale);
						const std::string label = Utils::AssetTileLabel(std::string(name), textWidth);
						const std::string detail = Utils::AssetTileLabel(std::string(type), textWidth);
						draw->PushClipRect(minimum, maximum, true);
						draw->AddText(ImVec2(minimum.x + inset, minimum.y + labelOffset), ImGui::GetColorU32(ImGuiCol_Text), label.c_str());
						draw->AddText(ImVec2(minimum.x + inset, minimum.y + labelOffset + ImGui::GetTextLineHeightWithSpacing()), ImGui::GetColorU32(ImGuiCol_TextDisabled), detail.c_str());
						if (issue)
							draw->AddCircleFilled(ImVec2(maximum.x - 9.0f * scale, minimum.y + 9.0f * scale), 4.0f * scale, IM_COL32(224, 161, 83, 255));
						draw->PopClipRect();
					};
					if (folders)
						for (const auto& entry : *folders)
						{
							if (!entry.Info.IsDirectory || entry.Path.GetParent().GetPath() != m_Directory || !Utils::AssetSearchKey(std::string(entry.Path.GetFileName())).contains(search))
								continue;
							++visibleFolders;
							ImGui::TableNextColumn();
							ImGui::PushID(static_cast<int>(FNV1a32(entry.Path.GetPath())));
							const ImVec2 minimum = ImGui::GetCursorScreenPos();
							if (ImGui::Selectable("##FolderTile", false, ImGuiSelectableFlags_AllowDoubleClick, ImVec2(0.0f, tileHeight)) && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
								nextDirectory = entry.Path.GetPath();
							const bool hovered = ImGui::IsItemHovered();
							drawTile(entry.Path.GetFileName(), "Folder", EditorIcon::Folder, 0, minimum, false, hovered, false);
							moveDrop(entry.Path.GetPath());
							if (hovered)
								ImGui::SetTooltip("%s\nDouble-click to open", std::string(entry.Path.GetPath()).c_str());
							ImGui::PopID();
						}
					std::vector<AssetHandle> handles = editor.GetAssets().GetRegistry().GetHandles();
					std::sort(handles.begin(), handles.end(), [&editor](AssetHandle left, AssetHandle right)
					{
						const auto leftPath = editor.GetAssets().GetReferencePath(left);
						const auto rightPath = editor.GetAssets().GetReferencePath(right);
						return leftPath == rightPath ? left < right : leftPath < rightPath;
					});
					for (const AssetHandle handle : handles)
					{
						const auto location = editor.GetAssets().GetRegistry().Locate(handle);
						if (!location || location->Record->SourcePath.GetParent().GetPath() != m_Directory)
							continue;
						const std::string path = editor.GetAssets().GetReferencePath(handle);
						const std::string type = Utils::EditorLabel(AssetTypeToString(location->Type));
						if (!Utils::AssetSearchKey(path + " " + type).contains(search))
							continue;
						++visibleAssets;
						ImGui::TableNextColumn();
						const std::string id = handle.ToString();
						ImGui::PushID(id.c_str());
						const ImVec2 minimum = ImGui::GetCursorScreenPos();
						if (ImGui::Selectable("##Asset", editor.GetUiState().GetSelectedAsset() == handle, ImGuiSelectableFlags_AllowDoubleClick, ImVec2(0.0f, tileHeight)))
						{
							editor.SetSelection({});
							editor.GetUiState().SetSelectedAsset(handle);
							if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && location->Type == AssetType::Scene)
								submit("scene.open", Json{ { "path", path } });
						}
						const bool hovered = ImGui::IsItemHovered();
						const auto diagnostic = std::find_if(editor.GetAssets().GetDiagnostics().begin(), editor.GetAssets().GetDiagnostics().end(), [&location](const auto& issue)
						{
							return issue.Path == location->Record->SourcePath.GetPath();
						});
						const bool hasIssue = diagnostic != editor.GetAssets().GetDiagnostics().end();
						uint64_t texture = 0;
						const ThumbnailRequest request{ context.Thumbnails.GetProjectGeneration(), handle, editor.GetAssets().GetVersion(handle), 128 };
						if (request.ProjectGeneration != 0)
						{
							const auto cached = context.Thumbnails.Find(request);
							if (!cached)
								m_Error = cached.error().GetMessageText();
							else if (!*cached)
							{
								if (const Status queued = context.Thumbnails.Queue(request); !queued)
									m_Error = queued.error().GetMessageText();
							}
							else if (!(*cached)->TypeIcon && context.FindThumbnailTexture)
								texture = context.FindThumbnailTexture(request);
						}
						const std::string label = location->SubAssetKey.empty() ? std::string(location->Record->SourcePath.GetFileName()) : location->SubAssetKey;
						drawTile(label, type, AssetTypeToEditorIcon(location->Type), texture, minimum, editor.GetUiState().GetSelectedAsset() == handle, hovered, hasIssue);
						if (hovered)
						{
							ImGui::BeginTooltip();
							ImGui::TextUnformatted(label.c_str());
							ImGui::TextDisabled("%s", type.c_str());
							ImGui::TextUnformatted(path.c_str());
							if (hasIssue)
								ImGui::TextWrapped("Import issue: %s", diagnostic->Message.c_str());
							ImGui::EndTooltip();
						}
						if (ImGui::BeginDragDropSource())
						{
							ImGui::SetDragDropPayload("ENGINE_ASSET", id.c_str(), id.size() + 1);
							ImGui::TextUnformatted(label.c_str());
							ImGui::EndDragDropSource();
						}
						if (ImGui::BeginPopupContextItem("AssetActions"))
						{
							editor.SetSelection({});
							editor.GetUiState().SetSelectedAsset(handle);
							ImGui::TextUnformatted(label.c_str());
							ImGui::Separator();
							if (location->Type == AssetType::Scene && ImGui::MenuItem("Open scene"))
								submit("scene.open", Json{ { "path", path } });
							ImGui::BeginDisabled(editor.IsReadOnly() || !location->SubAssetKey.empty());
							if (ImGui::MenuItem("Rename..."))
								requestDialog(handle, 1);
							if (ImGui::MenuItem("Move to folder..."))
								requestDialog(handle, 2);
							if (ImGui::MenuItem("Reimport"))
								submit("asset.reimport", Json{ { "asset", id } });
							ImGui::Separator();
							if (ImGui::MenuItem("Move to trash..."))
								requestDialog(handle, 3);
							ImGui::EndDisabled();
							ImGui::EndPopup();
						}
						ImGui::PopID();
					}
					ImGui::EndTable();
				}
				if (visibleAssets + visibleFolders == 0)
					Utils::EditorEmptyState(search.empty() ? "Make room for your ideas" : "No matching assets", search.empty() ? "Create an asset or import a file to get started. You can also drop files here." : "Try a different name or asset type, or clear the search.");
				if (nextDirectory)
					m_Directory = *nextDirectory;
			}
			ImGui::EndChild();
			ImGui::EndTable();
		}
		ImGui::Separator();
		const AssetHandle selected = editor.GetUiState().GetSelectedAsset();
		const auto selectedLocation = editor.GetAssets().GetRegistry().Locate(selected);
		if (selectedLocation)
		{
			const bool audio = selectedLocation->Type == AssetType::AudioClip;
			if (ImGui::Button("Actions"))
				ImGui::OpenPopup("SelectedActions");
			if (ImGui::BeginPopup("SelectedActions"))
			{
				ImGui::BeginDisabled(editor.IsReadOnly() || !selectedLocation->SubAssetKey.empty());
				if (ImGui::MenuItem("Rename..."))
					requestDialog(selected, 1);
				if (ImGui::MenuItem("Move to folder..."))
					requestDialog(selected, 2);
				if (ImGui::MenuItem("Reimport"))
					submit("asset.reimport", Json{ { "asset", selected.ToString() } });
				if (ImGui::MenuItem("Move to trash..."))
					requestDialog(selected, 3);
				ImGui::EndDisabled();
				ImGui::EndPopup();
			}
			if (audio)
			{
				ImGui::SameLine();
				ImGui::BeginDisabled(!context.QueueAudioPreview);
				if (Utils::EditorToolbarButton("Preview", "Listen to the selected audio asset"))
					if (const Status queued = context.QueueAudioPreview(selected); !queued)
						showError(queued.error());
				ImGui::SameLine();
				if (Utils::EditorToolbarButton("Stop", "Stop audio preview"))
					if (const Status queued = context.QueueAudioPreview({}); !queued)
						showError(queued.error());
				ImGui::EndDisabled();
			}
			ImGui::SameLine();
			const std::string path = editor.GetAssets().GetReferencePath(selected);
			ImGui::TextDisabled("%s", Utils::AssetTileLabel(path, std::max(1.0f, ImGui::GetContentRegionAvail().x)).c_str());
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("%s", path.c_str());
		}
		else
			ImGui::TextDisabled("%zu assets%s", visibleAssets, editor.IsReadOnly() ? "  /  Read only" : "");
		if (!m_Tickets.empty())
		{
			ImGui::TextDisabled("%zu operations pending", m_Tickets.size());
			ImGui::SameLine();
			if (ImGui::SmallButton("Cancel"))
				for (const uint64_t ticket : m_Tickets)
					if (const Status cancelled = context.Actions.Cancel(ticket); !cancelled)
						showError(cancelled.error());
		}
		if (!m_Error.empty())
			ImGui::TextWrapped("%s", m_Error.c_str());

		const float popupWidth = std::min(440.0f * scale, ImGui::GetMainViewport()->WorkSize.x * 0.9f);
		ImGui::SetNextWindowSizeConstraints(ImVec2(popupWidth, 0.0f), ImVec2(popupWidth, ImGui::GetMainViewport()->WorkSize.y * 0.9f));
		if (ImGui::BeginPopup("Create asset"))
		{
			Utils::EditorSectionHeading("Create asset");
			constexpr const char* Labels[] = { "Folder", "Scene", "Material", "Prefab", "Sound effect", "Behaviour script", "Module script", "Test script" };
			constexpr const char* Types[] = { "Folder", "Scene", "Material", "Prefab", "SoundEffect" };
			constexpr const char* Extensions[] = { "", ".scene", ".material", ".prefab", ".sfx", ".luau", ".luau", ".test.luau" };
			ImGui::TextDisabled("In %s", m_ActionDirectory.c_str());
			ImGui::SetNextItemWidth(-1.0f);
			ImGui::Combo("##AssetType", &m_CreateType, Labels, static_cast<int>(std::size(Labels)));
			ImGui::SetNextItemWidth(-1.0f);
			ImGui::InputTextWithHint("##AssetName", "Asset name", m_CreateName.data(), m_CreateName.size());
			ImGui::BeginDisabled(editor.IsReadOnly() || !Utils::AssetNameValid(m_CreateName.data()));
			if (ImGui::Button("Create asset"))
			{
				std::string name = m_CreateName.data();
				if (!name.ends_with(Extensions[m_CreateType]))
					name += Extensions[m_CreateType];
				bool queued = false;
				if (m_CreateType >= 5)
				{
					constexpr const char* Templates[] = { "Behaviour", "Module", "Test" };
					queued = submit("script.create", Json{ { "path", m_ActionDirectory + "/" + name }, { "template", Templates[m_CreateType - 5] } });
				}
				else
				{
					Json params{ { "type", Types[m_CreateType] }, { "path", m_ActionDirectory + "/" + name } };
					if (m_CreateType == 4)
						params["values"] = Json{ { "Layers", Json::array({ Json{ { "Duration", 0.1 } } }) } };
					queued = submit("asset.create", params);
				}
				if (queued)
					ImGui::CloseCurrentPopup();
			}
			ImGui::EndDisabled();
			ImGui::SameLine();
			if (ImGui::Button("Cancel"))
				ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
		}
		if (m_ImportPicker.IsOpen())
		{
			const auto picked = m_ImportPicker.Draw();
			if (!picked)
				showError(picked.error());
			else if (*picked)
			{
				const std::string path = FileSystem::PathToUtf8(**picked);
				if (path.size() >= m_ImportSource.size())
					showError(Error(ErrorCode::InvalidArgument, "The selected file path is too long."));
				else
				{
					m_ImportSource.fill(0);
					std::copy(path.begin(), path.end(), m_ImportSource.begin());
					m_ReopenImport = true;
				}
			}
		}
		if (m_ReopenImport)
		{
			ImGui::OpenPopup("Import asset");
			m_ReopenImport = false;
		}
		ImGui::SetNextWindowSizeConstraints(ImVec2(popupWidth, 0.0f), ImVec2(popupWidth, ImGui::GetMainViewport()->WorkSize.y * 0.9f));
		if (ImGui::BeginPopup("Import asset"))
		{
			Utils::EditorSectionHeading("Import a file");
			ImGui::TextWrapped("Add a model, image, audio clip or another supported asset to %s.", m_ImportDirectory.c_str());
			ImGui::BeginDisabled(editor.IsReadOnly());
			if (ImGui::Button("Browse files..."))
			{
				if (const Status opened = m_ImportPicker.OpenFile("Choose a file to import", editor.GetProject().GetRoot()); !opened)
					showError(opened.error());
				else
					ImGui::CloseCurrentPopup();
			}
			ImGui::TextDisabled("Or paste a file path");
			ImGui::SetNextItemWidth(-1.0f);
			ImGui::InputTextWithHint("##ImportSource", "Full path to a file", m_ImportSource.data(), m_ImportSource.size());
			ImGui::BeginDisabled(m_ImportSource[0] == '\0');
			if (ImGui::Button("Import file") && submit("asset.import", Json{ { "source", m_ImportSource.data() }, { "destDir", m_ImportDirectory } }))
				ImGui::CloseCurrentPopup();
			ImGui::EndDisabled();
			ImGui::EndDisabled();
			ImGui::SameLine();
			if (ImGui::Button("Cancel"))
				ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
		}
		if (m_AssetDialog != 0)
		{
			ImGui::OpenPopup(m_AssetDialog == 1 ? "Rename asset" : m_AssetDialog == 2 ? "Move asset"
																					  : "Trash asset");
			m_AssetDialog = 0;
		}
		for (int dialog = 1; dialog <= 3; ++dialog)
		{
			ImGui::SetNextWindowSizeConstraints(ImVec2(popupWidth, 0.0f), ImVec2(popupWidth, ImGui::GetMainViewport()->WorkSize.y * 0.9f));
			if (!ImGui::BeginPopup(dialog == 1 ? "Rename asset" : dialog == 2 ? "Move asset"
																			  : "Trash asset"))
				continue;
			const auto location = editor.GetAssets().GetRegistry().Locate(m_ActionAsset);
			if (!location || editor.IsReadOnly() || !location->SubAssetKey.empty())
				ImGui::CloseCurrentPopup();
			else
			{
				const std::string filename(location->Record->SourcePath.GetFileName());
				Utils::EditorSectionHeading(dialog == 1 ? "Rename asset" : dialog == 2 ? "Move to folder"
																					   : "Move to trash");
				ImGui::TextWrapped("%s", filename.c_str());
				if (dialog == 3)
					ImGui::TextWrapped("This asset will be moved to the project trash. You can restore it with Undo.");
				else if (dialog == 1)
				{
					ImGui::SetNextItemWidth(-1.0f);
					ImGui::InputTextWithHint("##Rename", "Filename including extension", m_MovePath.data(), m_MovePath.size());
				}
				else if (ImGui::BeginChild("Destinations", ImVec2(0.0f, ImGui::GetFrameHeightWithSpacing() * 6.0f), ImGuiChildFlags_Borders))
				{
					const auto destination = [this](std::string_view path)
					{
						const std::string text(path);
						// Hash every authored byte separately: ## and ### are valid path characters.
						ImGui::PushID(static_cast<int>(FNV1a32(path)));
						const ImVec2 minimum = ImGui::GetCursorScreenPos();
						if (ImGui::Selectable("##Destination", path == m_MovePath.data(), ImGuiSelectableFlags_None, ImVec2(0.0f, ImGui::GetFrameHeight())))
						{
							m_MovePath.fill(0);
							std::copy_n(text.begin(), std::min(text.size(), m_MovePath.size() - 1), m_MovePath.begin());
						}
						const ImVec2 maximum = ImGui::GetItemRectMax();
						const ImVec2 padding = ImGui::GetStyle().FramePadding;
						const std::string label = Utils::AssetTileLabel(text, std::max(1.0f, maximum.x - minimum.x - padding.x * 2.0f));
						ImGui::GetWindowDrawList()->AddText(ImVec2(minimum.x + padding.x, minimum.y + padding.y), ImGui::GetColorU32(ImGuiCol_Text), label.c_str());
						if (ImGui::IsItemHovered())
							ImGui::SetTooltip("%s", text.c_str());
						ImGui::PopID();
					};
					destination("Assets");
					if (folders)
						for (const auto& entry : *folders)
							if (entry.Info.IsDirectory)
								destination(entry.Path.GetPath());
				}
				if (dialog == 2)
					ImGui::EndChild();
				ImGui::BeginDisabled(dialog == 1 && !Utils::AssetNameValid(m_MovePath.data()));
				if (ImGui::Button(dialog == 1 ? "Rename" : dialog == 2 ? "Move"
																	   : "Move to trash"))
				{
					const std::string path = dialog == 1 ? std::string(location->Record->SourcePath.GetParent().GetPath()) + "/" + m_MovePath.data() : std::string(m_MovePath.data()) + "/" + filename;
					const bool queued = dialog == 3 ? submit("asset.delete", Json{ { "asset", m_ActionAsset.ToString() } }) : submit("asset.move", Json{ { "asset", m_ActionAsset.ToString() }, { "path", path } });
					if (queued)
						ImGui::CloseCurrentPopup();
				}
				ImGui::EndDisabled();
				ImGui::SameLine();
				if (ImGui::Button("Cancel"))
					ImGui::CloseCurrentPopup();
			}
			ImGui::EndPopup();
		}
		ImGui::PopID();
		return {};
	}

}
