#include "EditorPCH.h"
#include "Editor/Tools/TerrainHeightmapImport.h"

#include "Editor/EditorPanelContext.h"
#include "EditorCore/EditorActions.h"
#include "EditorCore/EditorContext.h"
#include "EditorCore/Play/EditorPlayController.h"
#include "EditorCore/Project/ProjectManager.h"
#include "Engine/Asset/AssetMetadata.h"
#include "Engine/Asset/AssetRegistry.h"
#include "Engine/AssetPipeline/EditorAssetManager.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/VfsPath.h"
#include "Engine/Core/VirtualFileSystem.h"
#include "Engine/Scene/Components/TerrainComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"

#include <imgui.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <format>
#include <utility>

namespace Engine {

	namespace {

		// The most `-<n>` suffixes the action tries for a free heightfield name.
		constexpr uint32_t MaxHeightfieldNameSuffix = 10000;

	}

	Result<TerrainHeightmapImportTarget> FindTerrainHeightmapImportTarget(const EditorContext& editor, const TerrainComponent& terrain, std::string_view source)
	{
		const EditorAssetManager& assets = editor.GetAssets();
		if (const AssetHandle handle = terrain.Heightfield.GetHandle(); handle.IsValid())
		{
			const AssetRecord* record = assets.GetRegistry().Find(handle);
			if (record != nullptr && record->Metadata.Kind == AssetMetaKind::Asset && record->Metadata.Handle == handle
				&& record->Metadata.Type == AssetType::Heightfield)
				return TerrainHeightmapImportTarget{ .Path = std::string(record->SourcePath.GetPath()), .Assign = false };
		}
		const std::string stem = FileSystem::PathToUtf8(FileSystem::PathFromUtf8(source).stem());
		if (stem.empty())
			return MakeError(ErrorCode::InvalidArgument, "'{}' names no heightmap file", source);
		const VirtualFileSystem& vfs = editor.GetVfs();
		for (uint32_t suffix = 1; suffix <= MaxHeightfieldNameSuffix; ++suffix)
		{
			const std::string relative = suffix == 1 ? std::format("Assets/Terrain/{}.heightfield", stem) : std::format("Assets/Terrain/{}-{}.heightfield", stem, suffix);
			ENGINE_TRY_ASSIGN(const VfsPath path, VfsPath::Create("project", relative));
			ENGINE_TRY_ASSIGN(const VfsPath meta, GetMetaPath(path));
			if (!vfs.Exists(path) && !vfs.Exists(meta))
				return TerrainHeightmapImportTarget{ .Path = relative, .Assign = true };
		}
		return MakeError(ErrorCode::AlreadyExists, "every heightfield name for '{}' in Assets/Terrain is taken", stem);
	}

	Status TerrainHeightmapImport::CollectResults(EditorPanelContext& context)
	{
		if (m_Import.has_value())
		{
			ENGINE_TRY_ASSIGN(std::optional<Result<Json>> completion, context.Actions.TakeResult(*m_Import));
			if (completion.has_value())
			{
				m_Import.reset();
				if (!completion->has_value())
					return std::unexpected(std::move(*completion).error());
				if (m_Assign)
				{
					// The imported heightfield's handle, as terrain.importHeightmap reports it.
					const JsonReader result(**completion);
					ENGINE_TRY_ASSIGN(const JsonReader asset, result.GetMember("asset"));
					ENGINE_TRY_ASSIGN(const std::string id, asset.ReadMember<std::string>("id"));
					const Json params{ { "entity", m_ImportEntity.ToString() }, { "components", Json{ { "Terrain", Json{ { "Heightfield", id } } } } } };
					ENGINE_TRY_ASSIGN(const uint64_t ticket, context.Actions.Submit("entity.update", params));
					m_Assignment = ticket;
				}
			}
		}
		if (m_Assignment.has_value())
		{
			ENGINE_TRY_ASSIGN(std::optional<Result<Json>> completion, context.Actions.TakeResult(*m_Assignment));
			if (completion.has_value())
			{
				m_Assignment.reset();
				if (!completion->has_value())
					return std::unexpected(std::move(*completion).error());
			}
		}
		return {};
	}

	Status TerrainHeightmapImport::Draw(EditorPanelContext& context, UUID entity)
	{
		EditorContext& editor = context.Editor;
		if (m_DialogEntity != entity && m_Dialog.IsOpen())
			m_Dialog.Cancel();
		Status status = CollectResults(context);

		const bool pending = m_Import.has_value() || m_Assignment.has_value();
		ImGui::BeginDisabled(editor.IsReadOnly() || editor.GetPlay().IsPlaying() || pending || !editor.HasProject());
		if (ImGui::Button("Import heightmap..."))
		{
			const FileDialogRequest request = {
				.Kind = FileDialogKind::OpenFile,
				.Title = "Choose a 16-bit heightmap",
				.InitialDirectory = editor.GetProject().GetRoot(),
				.Extensions = { ".png", ".raw", ".r16" },
				.FilterName = "Heightmaps",
			};
			if (const Status opened = m_Dialog.Open(request, context.FileDialogs); !opened && status)
				status = std::unexpected(opened.error());
			m_DialogEntity = entity;
		}
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
		{
			ImGui::SetTooltip("Convert a 16-bit PNG or a RAW heightmap (at most 1025 x 1025 samples) into this terrain's heightfield "
							  "(terrain.importHeightmap; undoable)");
		}
		ImGui::EndDisabled();
		if (!m_Dialog.IsOpen())
			return status;

		ENGINE_TRY(status);
		ENGINE_TRY_ASSIGN(const std::optional<std::filesystem::path> picked, m_Dialog.Draw());
		if (!picked.has_value())
			return {};
		// The terrain the dialog was opened for, as it is now.
		const Scene* scene = editor.HasScene() ? &editor.GetScene() : nullptr;
		const ConstEntity target = scene != nullptr ? scene->FindEntityByID(m_DialogEntity) : ConstEntity();
		const TerrainComponent* terrain = target.IsValid() ? target.TryGetComponent<TerrainComponent>() : nullptr;
		if (terrain == nullptr)
			return MakeError(ErrorCode::NotFound, "The terrain the heightmap was chosen for is no longer in the scene");
		const std::string source = FileSystem::PathToUtf8(*picked);
		ENGINE_TRY_ASSIGN(const TerrainHeightmapImportTarget importTarget, FindTerrainHeightmapImportTarget(editor, *terrain, source));
		ENGINE_TRY_ASSIGN(const uint64_t ticket, context.Actions.Submit("terrain.importHeightmap", Json{ { "source", source }, { "path", importTarget.Path } }));
		m_Import = ticket;
		m_ImportEntity = m_DialogEntity;
		m_Assign = importTarget.Assign;
		return {};
	}

}
