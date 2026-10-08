#include "TestsPCH.h"
#include "Support/GoldenScene.h"

#include "EditorCore/EngineAssetGenerators.h"
#include "Engine/Asset/AssetDiagnostic.h"
#include "Engine/AssetPipeline/EditorAssetManager.h"
#include "Engine/Core/Error.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Mounts/NativeDirectoryMount.h"
#include "Engine/Core/VfsPath.h"
#include "Engine/Core/VirtualFileSystem.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Renderer/EnvironmentBaker.h"
#include "Engine/Renderer/GpuResourceCache.h"
#include "Engine/Renderer/SceneRenderer.h"
#include "Engine/Renderer/ViewportCapture.h"
#include "Engine/Scene/LoadReport.h"
#include "Engine/Scene/RenderExtraction.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/SceneSerializer.h"
#include "Engine/Scene/TransformSystem.h"
#include "Support/AssetTestFixture.h"
#include "Support/HeadlessGpuFixture.h"
#include "Support/TempDirectory.h"
#include "Support/TestData.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// RenderGoldenScene (GoldenScene.h) renders a golden scene the way viewport.screenshot renders an edit scene's game view,
// over a disposable copy of the FeatureTest project, so opening it (which may write .meta files and cooks into cache://)
// never touches the repository. A golden scene must load and render cleanly: any load diagnostic, scan diagnostic or asset
// diagnostic of Error severity fails the render instead of producing an image with placeholders in it.

namespace Engine {

	namespace Test {

		namespace {

			// The project's folder of golden scenes, project-relative.
			constexpr std::string_view GoldenSceneFolder = "Assets/Scenes/Golden";

			// A golden scene name is a plain file stem: letters, digits, '-' and '_'.
			bool IsGoldenSceneName(std::string_view name)
			{
				return !name.empty() && std::ranges::all_of(name, [](char character)
				{
					return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') || (character >= '0' && character <= '9')
						|| character == '-' || character == '_';
				});
			}

			std::filesystem::path GetFeatureTestRoot()
			{
				return GetRepositoryRoot() / "Projects" / "FeatureTest";
			}

			// Copies the project at `source` into the existing directory `destination`, without its Library/ (the editor's
			// cache, autosave and lock).
			Status CopyProject(const std::filesystem::path& source, const std::filesystem::path& destination)
			{
				ENGINE_TRY_ASSIGN(const std::vector<std::filesystem::path> entries, FileSystem::ListDirectory(source, true));
				for (const std::filesystem::path& entry : entries)
				{
					const std::filesystem::path relative = entry.lexically_relative(source);
					if (relative.empty() || *relative.begin() == "Library")
						continue;
					ENGINE_TRY_ASSIGN(const FileInfo info, FileSystem::GetInfo(entry));
					const std::filesystem::path target = destination / relative;
					if (info.IsDirectory)
					{
						ENGINE_TRY(FileSystem::CreateDirectories(target));
						continue;
					}
					ENGINE_TRY(FileSystem::CreateDirectories(target.parent_path()));
					ENGINE_TRY_ASSIGN(const Buffer bytes, FileSystem::ReadFile(entry));
					ENGINE_TRY(FileSystem::WriteFileAtomic(target, bytes, { .KeepBackup = false }));
				}
				return {};
			}

			// The diagnostics of a load report, joined with '; '.
			std::string DescribeLoadDiagnostics(const LoadReport& report)
			{
				std::string text;
				for (const LoadDiagnostic& diagnostic : report.Diagnostics)
					text += std::format("{}{} at '{}': {}", text.empty() ? "" : "; ", diagnostic.Code, diagnostic.JsonPointer, diagnostic.Message);
				return text;
			}

			// The asset diagnostics of Error severity, joined with '; '; empty when there is none.
			std::string DescribeAssetErrors(std::span<const AssetDiagnostic> diagnostics)
			{
				std::string text;
				for (const AssetDiagnostic& diagnostic : diagnostics)
				{
					if (diagnostic.Severity == DiagnosticSeverity::Error)
						text += std::format("{}{} '{}': {}", text.empty() ? "" : "; ", diagnostic.Code, diagnostic.Path, diagnostic.Message);
				}
				return text;
			}

			// The render itself, over the project copy at `projectRoot`; every GPU object it creates is gone when it returns.
			Result<Image> RenderProjectCopy(HeadlessGpuFixture& gpu, const std::filesystem::path& projectRoot, std::string_view name,
				const GoldenSceneOptions& options)
			{
				GraphicsDevice& device = gpu.GetDevice();
				ENGINE_TRY_ASSIGN(const Scope<EnvironmentBaker> baker, EnvironmentBaker::Create(device, gpu.GetPipelines()));

				// The editor's asset environment over the copy: the engine resources with an in-memory engine cache, the
				// baker for the built-in environments and the generators of the Generated built-ins.
				AssetTestFixture assets({ .EngineResources = true, .EnvironmentBaker = baker.get(), .EngineAssetGenerators = GetEngineAssetGenerators() });
				VirtualFileSystem& vfs = assets.GetVfs();
				ENGINE_TRY(vfs.Unmount("project"));
				ENGINE_TRY(vfs.Mount("project", CreateScope<NativeDirectoryMount>(projectRoot)));
				ENGINE_TRY_ASSIGN(const VfsPath assetsRoot, VfsPath::Create("project", "Assets"));
				ENGINE_TRY_ASSIGN(const VfsPath cacheRoot, VfsPath::Parse("cache://"));
				ENGINE_TRY_ASSIGN(const AssetRefreshReport refresh,
					assets.GetManager().OpenProject({ .AssetsRoot = assetsRoot, .CacheRoot = cacheRoot, .ReadOnly = false, .HotReload = false }));
				if (!refresh.CreatedMetas.empty())
				{
					const std::string message = std::format("Projects/FeatureTest has {} source(s) without a .meta, the first '{}'", refresh.CreatedMetas.size(),
						refresh.CreatedMetas.front().ToString());
					return std::unexpected(Error(ErrorCode::Validation, message).WithHint("write the project's assets through its scaffolds, which create every .meta"));
				}

				// The scene, strictly: unknown keys, fields and components are errors, and so is any other diagnostic.
				const std::string relative = std::format("{}/{}.scene", GoldenSceneFolder, name);
				ENGINE_TRY_ASSIGN(const VfsPath scenePath, VfsPath::Create("project", relative));
				SceneSpecification specification;
				specification.Name = std::string(name);
				specification.Registry = &assets.GetRegistry();
				specification.IdGenerator = &assets.GetGenerator();
				const Scope<Scene> scene = Scene::Create(specification);
				LoadReport report;
				ENGINE_TRY(WithContext(SceneSerializer::LoadFromFile(*scene, vfs, scenePath, { .StrictUnknowns = true, .SourcePath = relative }, report),
					std::format("loading the golden scene '{}'", name)));
				if (!report.Diagnostics.empty())
					return MakeError(ErrorCode::Validation, "the golden scene '{}' loads with diagnostics: {}", name, DescribeLoadDiagnostics(report));
				TransformSystem::Update(*scene);

				GpuResourceCache cache(device, assets.GetManager());
				ENGINE_TRY_ASSIGN(const Scope<SceneRendererPipelines> pipelines, SceneRendererPipelines::Create(device, gpu.GetPipelines()));
				ENGINE_TRY_ASSIGN(const Scope<ViewportCapture> capture, ViewportCapture::CreateForScenes(device, *pipelines, cache, assets.GetManager()));

				// The game view at the requested size, as viewport.screenshot {view: "game"} extracts it.
				ENGINE_TRY_ASSIGN(RenderSnapshot snapshot, ExtractRenderSnapshot(*scene, { .Width = options.Width, .Height = options.Height }));
				if (!snapshot.HasCamera)
					return MakeError(ErrorCode::Validation, "the golden scene '{}' has no primary camera", name);
				if (options.EditSnapshot)
					options.EditSnapshot(snapshot);
				assets.GetManager().WaitIdle();
				ENGINE_TRY_ASSIGN(Image image, capture->Capture({ .Width = options.Width, .Height = options.Height }, snapshot));

				const std::string errors = DescribeAssetErrors(assets.GetManager().GetDiagnostics());
				if (!errors.empty())
					return MakeError(ErrorCode::Validation, "the golden scene '{}' rendered with asset errors: {}", name, errors);
				return image;
			}

		}

		Result<Image> RenderGoldenScene(HeadlessGpuFixture& gpu, std::string_view name, const GoldenSceneOptions& options)
		{
			const std::filesystem::path source = GetFeatureTestRoot();
			if (!IsGoldenSceneName(name) || !FileSystem::Exists(source / FileSystem::PathFromUtf8(std::format("{}/{}.scene", GoldenSceneFolder, name))))
			{
				return std::unexpected(Error(ErrorCode::NotFound, std::format("'{}' is not a golden scene of Projects/FeatureTest/{}", name, GoldenSceneFolder))
						.WithHint("golden scenes are written by Projects/FeatureTest/Scaffold/Golden.jsonl"));
			}

			const TempDirectory copy("GoldenScene");
			ENGINE_TRY(WithContext(CopyProject(source, copy.GetPath()), "copying Projects/FeatureTest"));
			Result<Image> image = RenderProjectCopy(gpu, copy.GetPath(), name, options);
			// The render's objects are released; let the device destroy them now rather than at its next collection.
			gpu.GetDevice().RunGarbageCollection();
			return image;
		}

		Result<Image> ComposeImageGrid(std::span<const Image> images, uint32_t columns)
		{
			if (images.empty() || columns == 0)
				return MakeError(ErrorCode::InvalidArgument, "an image grid needs images ({}) and columns ({})", images.size(), columns);
			const Image& first = images.front();
			for (size_t index = 0; index < images.size(); ++index)
			{
				const Image& image = images[index];
				if (!image.IsValid() || image.Width != first.Width || image.Height != first.Height || image.Format != first.Format)
				{
					return MakeError(ErrorCode::InvalidArgument, "image {} of the grid ({}x{}) does not match the first ({}x{}) or is invalid", index, image.Width,
						image.Height, first.Width, first.Height);
				}
			}

			// Cells past the last image (a last row that is not full) stay zero.
			const uint32_t count = static_cast<uint32_t>(images.size());
			const uint32_t rows = (count + columns - 1) / columns;
			ENGINE_TRY_ASSIGN(Image grid, CreateImage(first.Width * columns, first.Height * rows, first.Format));
			const size_t cellPitch = first.GetRowPitch();
			const size_t gridPitch = grid.GetRowPitch();
			for (uint32_t index = 0; index < count; ++index)
			{
				const size_t cellX = index % columns;
				const size_t cellY = index / columns;
				for (uint32_t row = 0; row < first.Height; ++row)
				{
					const std::span<const std::byte> source = images[index].GetRow(row);
					const size_t offset = (cellY * first.Height + row) * gridPitch + cellX * cellPitch;
					std::ranges::copy(source, grid.Pixels.begin() + static_cast<std::ptrdiff_t>(offset));
				}
			}
			return grid;
		}

	}

}
