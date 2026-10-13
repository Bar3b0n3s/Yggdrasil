#include "TestsPCH.h"
#include "EditorCore/Thumbnails/ThumbnailCache.h"

#include "EditorCore/EditorContext.h"
#include "EditorCore/Project/ProjectManager.h"
#include "Engine/Asset/BuiltinAssets.h"
#include "Engine/Asset/MaterialData.h"
#include "Engine/AssetPipeline/EditorAssetManager.h"
#include "Engine/Core/Buffer.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Project/ProjectSettings.h"
#include "Support/AutomationTestClient.h"
#include "Support/EditorTestFixture.h"

#include <algorithm>
#include <string>
#include <vector>

namespace Engine {

	static ThumbnailRequest MakeContentThumbnailRequest(EditorContext& editor, ThumbnailCache& cache, AssetHandle asset = BuiltinAssetHandles::CubeMesh, uint32_t size = 32)
	{
		return { cache.GetProjectGeneration(), asset, editor.GetAssets().GetVersion(asset), size };
	}

	static Result<Image> RenderContentTestThumbnail(const ThumbnailRequest& request)
	{
		ENGINE_TRY_ASSIGN(Image image, CreateImage(request.Size, request.Size, nvrhi::Format::RGBA8_UNORM));
		std::fill(image.Pixels.begin(), image.Pixels.end(), std::byte(request.Size));
		return image;
	}

	static std::filesystem::path ThumbnailDirectory(const EditorContext& editor)
	{
		return editor.GetProject().GetCacheDirectory() / "Thumbnails";
	}

	// The file names in the project's thumbnail directory, sorted; empty when it does not exist.
	static std::vector<std::string> ListThumbnailFiles(const EditorContext& editor)
	{
		std::vector<std::string> names;
		if (const auto listed = FileSystem::ListDirectory(ThumbnailDirectory(editor)))
			for (const auto& path : *listed)
				names.push_back(FileSystem::PathToUtf8(path.filename()));
		return names;
	}

	// Writes a material source (its size depends on `roughness`'s digits) and lets the asset manager import it.
	static AssetHandle WriteThumbnailMaterial(EditorContext& editor, std::string_view path, float roughness, AssetHandle baseColorMap = {})
	{
		MaterialData material;
		material.Roughness = roughness;
		material.BaseColorMap = TypedAssetHandle<AssetType::Texture>(baseColorMap);
		const auto text = MaterialToText(material, editor.GetTypeRegistry());
		REQUIRE(text);
		REQUIRE(FileSystem::WriteFileAtomic(editor.GetProject().GetRoot() / path, AsBytes(*text)));
		REQUIRE(editor.GetAssets().Refresh());
		editor.GetAssets().WaitIdle();
		const auto handle = editor.GetAssets().Resolve(path);
		REQUIRE(handle);
		return *handle;
	}

	static AssetHandle WriteThumbnailTexture(EditorContext& editor, std::string_view path, uint32_t size, std::byte value)
	{
		auto image = CreateImage(size, size, nvrhi::Format::RGBA8_UNORM);
		REQUIRE(image);
		std::fill(image->Pixels.begin(), image->Pixels.end(), value);
		REQUIRE(FileSystem::CreateDirectories((editor.GetProject().GetRoot() / path).parent_path()));
		REQUIRE(WritePng(editor.GetProject().GetRoot() / path, *image));
		REQUIRE(editor.GetAssets().Refresh());
		editor.GetAssets().WaitIdle();
		const auto handle = editor.GetAssets().Resolve(path);
		REQUIRE(handle);
		return *handle;
	}

	// Closes and opens the fixture's project again: a new project epoch whose assets start unloaded.
	static void ReopenThumbnailProject(Test::EditorTestFixture& fixture)
	{
		auto& editor = fixture.GetEditor();
		const auto root = editor.GetProject().GetRoot();
		REQUIRE(editor.CloseProject());
		auto loaded = ProjectManager::OpenProject(root, {}, editor.GetTypeRegistry());
		REQUIRE(loaded);
		REQUIRE(editor.OpenProject(std::move(*loaded)));
	}

	TEST_SUITE("EditorCore")
	{
		TEST_CASE("ThumbnailCache: cache keys include asset version size and renderer format")
		{
			Test::AutomationFixture fixture("ThumbnailKeys");
			auto& editor = fixture.GetEditor();
			uint32_t renders = 0;
			ThumbnailCache cache(editor, [&renders](const ThumbnailRequest& request)
			{
				++renders;
				return RenderContentTestThumbnail(request);
			});
			REQUIRE(cache.BindProject(1));
			auto request = MakeContentThumbnailRequest(editor, cache);
			const auto first = cache.Request(request);
			REQUIRE(first);
			CHECK_FALSE(first->Cached);
			const auto firstFile = FileSystem::PathFromUtf8(first->Path);
			CHECK(firstFile.parent_path() == ThumbnailDirectory(editor));
			const std::string name = FileSystem::PathToUtf8(firstFile.filename());
			CHECK(name.starts_with(request.Asset.ToString() + "-32-"));
			CHECK(name.ends_with(".png"));
			const auto png = ReadPng(firstFile);
			REQUIRE(png);
			CHECK(png->Pixels == RenderContentTestThumbnail(request)->Pixels);
			CHECK(cache.Request(request)->Cached);
			CHECK(renders == 1);
			cache.Invalidate(request.Asset);
			CHECK_FALSE(cache.Find(request)->has_value());
			CHECK(cache.Request(request)->Cached);
			CHECK(renders == 1);
			request.Size = 64;
			CHECK(cache.Request(request)->Path != first->Path);
			CHECK(renders == 2);

			const auto created = fixture.Call("asset.create", Json{ { "type", "Material" }, { "path", "Assets/Materials/Preview.material" } });
			REQUIRE(created);
			const auto asset = JsonReader((*created)["asset"]["id"]).ReadUUID();
			REQUIRE(asset);
			REQUIRE(editor.GetAssets().Load(*asset));
			const auto beforeRequest = MakeContentThumbnailRequest(editor, cache, *asset);
			const auto before = cache.Request(beforeRequest);
			REQUIRE(before);
			REQUIRE(fixture.Call("asset.setProperties", Json{ { "asset", asset->ToString() }, { "values", Json{ { "Roughness", 0.2f } } } }));
			REQUIRE(editor.GetAssets().Refresh());
			editor.GetAssets().WaitIdle();
			const auto afterRequest = MakeContentThumbnailRequest(editor, cache, *asset);
			CHECK(afterRequest.Version > beforeRequest.Version);
			const auto after = cache.Request(afterRequest);
			REQUIRE(after);
			CHECK(after->Path != before->Path);
		}

		TEST_CASE("ThumbnailCache: failed rendering preserves the previous cached image")
		{
			Test::EditorTestFixture fixture("ThumbnailFailure");
			fixture.CreateAndOpenProject();
			bool fail = false;
			uint32_t calls = 0;
			ThumbnailCache cache(fixture.GetEditor(), [&fail, &calls](const ThumbnailRequest& request) -> Result<Image>
			{
				++calls;
				if (fail)
					return MakeError(ErrorCode::Io, "injected render failure");
				return RenderContentTestThumbnail(request);
			});
			REQUIRE(cache.BindProject(1));
			auto request = MakeContentThumbnailRequest(fixture.GetEditor(), cache);
			const auto first = cache.Request(request);
			REQUIRE(first);
			const auto original = FileSystem::ReadFile(FileSystem::PathFromUtf8(first->Path));
			REQUIRE(original);
			request.Size = 64;
			fail = true;
			CHECK(cache.Request(request).error().GetCode() == ErrorCode::Io);
			CHECK(cache.Find(request).error().GetCode() == ErrorCode::Io);
			CHECK(cache.Request(request).error().GetCode() == ErrorCode::Io);
			REQUIRE(cache.Queue(request));
			REQUIRE(cache.Pump());
			CHECK(calls == 2);
			const auto unchanged = FileSystem::ReadFile(FileSystem::PathFromUtf8(first->Path));
			REQUIRE(unchanged);
			CHECK(*unchanged == *original);
			cache.Invalidate(request.Asset);
			fail = false;
			CHECK(cache.Request(request));
			CHECK(calls == 3);
		}

		TEST_CASE("ThumbnailCache: read-only projects use their private cache")
		{
			Test::EditorTestFixture fixture("ThumbnailReadOnly");
			fixture.CreateAndOpenProject();
			auto& editor = fixture.GetEditor();
			const auto root = editor.GetProject().GetRoot();
			REQUIRE(editor.CloseProject());
			const auto privateCache = fixture.GetDirectory() / "PrivateThumbnailCache";
			auto loaded = ProjectManager::OpenProject(root, { .ReadOnly = true, .ReadOnlyCacheDirectory = privateCache }, editor.GetTypeRegistry());
			REQUIRE(loaded);
			REQUIRE(editor.OpenProject(std::move(*loaded)));
			ThumbnailCache cache(editor, RenderContentTestThumbnail);
			REQUIRE(cache.BindProject(1));
			auto result = cache.Request(MakeContentThumbnailRequest(editor, cache));
			REQUIRE(result);
			CHECK(FileSystem::PathFromUtf8(result->Path).parent_path() == privateCache / "Thumbnails");
			CHECK_FALSE(FileSystem::Exists(root / "Library" / "Cache" / "Thumbnails"));
			cache.Reset();
			REQUIRE(editor.CloseProject());
			CHECK_FALSE(FileSystem::Exists(privateCache));
		}

		TEST_CASE("ThumbnailCache: nonvisual assets use a type icon without a dummy image")
		{
			Test::EditorTestFixture fixture("ThumbnailTypeIcon");
			fixture.CreateAndOpenProject();
			uint32_t calls = 0;
			ThumbnailCache cache(fixture.GetEditor(), [&calls](const ThumbnailRequest& request)
			{
				++calls;
				return RenderContentTestThumbnail(request);
			});
			REQUIRE(cache.BindProject(1));
			auto result = cache.Request(MakeContentThumbnailRequest(fixture.GetEditor(), cache, BuiltinAssetHandles::SilentClip));
			REQUIRE(result);
			CHECK(result->TypeIcon);
			CHECK(result->Path.empty());
			CHECK(calls == 0);
			CHECK(ListThumbnailFiles(fixture.GetEditor()).empty());
		}

		TEST_CASE("ThumbnailCache: stale versions and invalid sizes fail without a cache write")
		{
			Test::EditorTestFixture fixture("ThumbnailInvalid");
			fixture.CreateAndOpenProject();
			ThumbnailCache cache(fixture.GetEditor(), RenderContentTestThumbnail);
			REQUIRE(cache.BindProject(1));
			auto request = MakeContentThumbnailRequest(fixture.GetEditor(), cache);
			++request.Version;
			CHECK(cache.Request(request).error().GetCode() == ErrorCode::Conflict);
			CHECK(cache.Queue(request).error().GetCode() == ErrorCode::Conflict);
			--request.Version;
			for (const uint32_t size : { 0u, 15u, 513u })
			{
				request.Size = size;
				CHECK(cache.Request(request).error().GetCode() == ErrorCode::InvalidArgument);
			}
			request.Size = 32;
			request.Asset = {};
			CHECK(cache.Request(request).error().GetCode() == ErrorCode::InvalidArgument);
			request.Asset = UUID(0xffffffffffffffff);
			CHECK(cache.Request(request).error().GetCode() == ErrorCode::NotFound);
			CHECK_FALSE(FileSystem::Exists(ThumbnailDirectory(fixture.GetEditor())));
		}

		TEST_CASE("ThumbnailCache: switching projects with matching asset identifiers never reuses an old image")
		{
			Test::EditorTestFixture fixture("ThumbnailSwitch");
			fixture.CreateAndOpenProject("First");
			auto& editor = fixture.GetEditor();
			uint32_t calls = 0;
			ThumbnailCache cache(editor, [&calls](const ThumbnailRequest& request)
			{
				++calls;
				return RenderContentTestThumbnail(request);
			});
			REQUIRE(cache.BindProject(1));
			const auto first = cache.Request(MakeContentThumbnailRequest(editor, cache));
			REQUIRE(first);
			cache.Reset();
			REQUIRE(editor.CloseProject());
			fixture.CreateAndOpenProject("Second");
			REQUIRE(cache.BindProject(2));
			const auto second = cache.Request(MakeContentThumbnailRequest(editor, cache));
			REQUIRE(second);
			CHECK(second->Path != first->Path);
			CHECK(FileSystem::PathFromUtf8(second->Path).parent_path() == ThumbnailDirectory(editor));
			CHECK(calls == 2);
		}

		TEST_CASE("ThumbnailCache: reset cancels old queued work before another project is bound")
		{
			Test::EditorTestFixture fixture("ThumbnailReset");
			fixture.CreateAndOpenProject("First");
			uint32_t calls = 0;
			ThumbnailCache cache(fixture.GetEditor(), [&calls](const ThumbnailRequest& request)
			{
				++calls;
				return RenderContentTestThumbnail(request);
			});
			REQUIRE(cache.BindProject(1));
			REQUIRE(cache.Queue(MakeContentThumbnailRequest(fixture.GetEditor(), cache)));
			cache.Reset();
			REQUIRE(fixture.GetEditor().CloseProject());
			fixture.CreateAndOpenProject("Second");
			REQUIRE(cache.BindProject(2));
			REQUIRE(cache.Pump());
			CHECK(calls == 0);
		}

		TEST_CASE("ThumbnailCache: stale request generations fail in request queue and find")
		{
			Test::EditorTestFixture fixture("ThumbnailStale");
			fixture.CreateAndOpenProject();
			ThumbnailCache cache(fixture.GetEditor(), RenderContentTestThumbnail);
			REQUIRE(cache.BindProject(1));
			const auto old = MakeContentThumbnailRequest(fixture.GetEditor(), cache);
			cache.Reset();
			REQUIRE(cache.BindProject(2));
			CHECK(cache.Request(old).error().GetCode() == ErrorCode::Conflict);
			CHECK(cache.Queue(old).error().GetCode() == ErrorCode::Conflict);
			CHECK(cache.Find(old).error().GetCode() == ErrorCode::Conflict);
		}

		TEST_CASE("ThumbnailCache: unbound and reused project generations fail without cache access")
		{
			Test::EditorTestFixture fixture("ThumbnailUnbound");
			ThumbnailCache cache(fixture.GetEditor(), RenderContentTestThumbnail);
			CHECK(cache.BindProject(1).error().GetCode() == ErrorCode::InvalidState);
			fixture.CreateAndOpenProject();
			const ThumbnailRequest request{ 1, BuiltinAssetHandles::CubeMesh, 0, 32 };
			CHECK(cache.Request(request).error().GetCode() == ErrorCode::InvalidState);
			CHECK(cache.Queue(request).error().GetCode() == ErrorCode::InvalidState);
			CHECK(cache.Find(request).error().GetCode() == ErrorCode::InvalidState);
			CHECK(cache.BindProject(0).error().GetCode() == ErrorCode::InvalidArgument);
			REQUIRE(cache.BindProject(2));
			CHECK(cache.BindProject(3).error().GetCode() == ErrorCode::InvalidState);
			CHECK(cache.GetProjectGeneration() == 2);
			cache.Reset();
			CHECK(cache.BindProject(1).error().GetCode() == ErrorCode::InvalidArgument);
			CHECK(cache.BindProject(2).error().GetCode() == ErrorCode::InvalidArgument);
			CHECK(cache.GetProjectGeneration() == 0);
		}

		TEST_CASE("ThumbnailCache: independent cache lifetimes share content keyed images")
		{
			Test::EditorTestFixture fixture("ThumbnailLifetimes");
			fixture.CreateAndOpenProject();
			uint32_t renders = 0;
			const auto render = [&renders](const ThumbnailRequest& request)
			{
				++renders;
				return RenderContentTestThumbnail(request);
			};
			ThumbnailCache first(fixture.GetEditor(), render);
			ThumbnailCache second(fixture.GetEditor(), render);
			REQUIRE(first.BindProject(1));
			REQUIRE(second.BindProject(1));
			const auto a = first.Request(MakeContentThumbnailRequest(fixture.GetEditor(), first));
			const auto b = second.Request(MakeContentThumbnailRequest(fixture.GetEditor(), second));
			REQUIRE(a);
			REQUIRE(b);
			CHECK(a->Path == b->Path);
			CHECK_FALSE(a->Cached);
			CHECK(b->Cached);
			CHECK(renders == 1);
			const std::string filename = FileSystem::PathToUtf8(FileSystem::PathFromUtf8(a->Path).filename());
			CHECK(filename.find_first_not_of("0123456789abcdef-.png") == std::string::npos);
		}

		TEST_CASE("ThumbnailCache: a reopened project reuses its cached image without rendering")
		{
			Test::EditorTestFixture fixture("ThumbnailReopen");
			fixture.CreateAndOpenProject();
			auto& editor = fixture.GetEditor();
			const AssetHandle material = WriteThumbnailMaterial(editor, "Assets/Reused.material", 0.25f);
			uint32_t renders = 0;
			ThumbnailCache cache(editor, [&renders](const ThumbnailRequest& request)
			{
				++renders;
				return RenderContentTestThumbnail(request);
			});
			REQUIRE(cache.BindProject(1));
			const auto first = cache.Request(MakeContentThumbnailRequest(editor, cache, material));
			REQUIRE(first);
			CHECK_FALSE(first->Cached);
			CHECK(renders == 1);
			const auto files = ListThumbnailFiles(editor);
			CHECK(files.size() == 2); // the image and its manifest

			for (uint64_t generation = 2; generation <= 4; ++generation)
			{
				INFO("reopen with generation ", generation);
				cache.Reset();
				ReopenThumbnailProject(fixture);
				REQUIRE(cache.BindProject(generation));
				const auto request = MakeContentThumbnailRequest(editor, cache, material);
				CHECK(editor.GetAssets().GetState(material) == AssetState::Unloaded); // a new epoch: nothing is loaded
				const auto reused = cache.Request(request);
				REQUIRE(reused);
				CHECK(reused->Cached);
				CHECK(reused->Path == first->Path);
				CHECK(renders == 1);
				CHECK(ListThumbnailFiles(editor) == files);
			}
		}

		TEST_CASE("ThumbnailCache: a changed input replaces its image and binding removes files nothing names")
		{
			Test::EditorTestFixture fixture("ThumbnailCleanup");
			fixture.CreateAndOpenProject();
			auto& editor = fixture.GetEditor();
			const AssetHandle material = WriteThumbnailMaterial(editor, "Assets/Changing.material", 0.25f);
			ThumbnailCache cache(editor, RenderContentTestThumbnail);
			REQUIRE(cache.BindProject(1));
			const auto first = cache.Request(MakeContentThumbnailRequest(editor, cache, material));
			REQUIRE(first);
			// A longer number changes the source's size as well as its time.
			REQUIRE(WriteThumbnailMaterial(editor, "Assets/Changing.material", 0.123456f) == material);
			const auto second = cache.Request(MakeContentThumbnailRequest(editor, cache, material));
			REQUIRE(second);
			CHECK_FALSE(second->Cached);
			CHECK(second->Path != first->Path);
			CHECK_FALSE(FileSystem::Exists(FileSystem::PathFromUtf8(first->Path)));
			const std::string image = FileSystem::PathToUtf8(FileSystem::PathFromUtf8(second->Path).filename());
			const std::string manifest = material.ToString() + "-32.json";
			CHECK(ListThumbnailFiles(editor) == std::vector<std::string>{ image, manifest });

			// Leftovers: the per-session naming of renderer format 1, an image nothing names, and an unknown asset's files.
			const auto cacheRoot = editor.GetProject().GetCacheDirectory();
			const auto legacy = cacheRoot / std::format("thumbnail-v1-0123456789abcdef-1-{}-1-32.png", material.ToString());
			const auto orphan = ThumbnailDirectory(editor) / (material.ToString() + "-32-0000000000000001.png");
			const auto unknownImage = ThumbnailDirectory(editor) / "00000000000000ff-32-0000000000000002.png";
			const auto unknownManifest = ThumbnailDirectory(editor) / "00000000000000ff-32.json";
			const auto foreign = ThumbnailDirectory(editor) / "notes.txt";
			for (const auto& file : { legacy, orphan, unknownImage, foreign })
				REQUIRE(FileSystem::WriteFileAtomic(file, AsBytes("x")));
			std::string unknownText = FileSystem::ReadText(ThumbnailDirectory(editor) / manifest).value_or(std::string());
			REQUIRE_FALSE(unknownText.empty());
			const size_t asset = unknownText.find(material.ToString());
			REQUIRE(asset != std::string::npos);
			unknownText.replace(asset, 16, "00000000000000ff");
			REQUIRE(FileSystem::WriteFileAtomic(unknownManifest, AsBytes(unknownText)));
			cache.Reset();
			REQUIRE(cache.BindProject(2));
			CHECK_FALSE(FileSystem::Exists(legacy));
			CHECK_FALSE(FileSystem::Exists(orphan));
			CHECK_FALSE(FileSystem::Exists(unknownImage));
			CHECK_FALSE(FileSystem::Exists(unknownManifest));
			CHECK(FileSystem::Exists(foreign)); // only thumbnail names are ever removed
			CHECK(ListThumbnailFiles(editor) == std::vector<std::string>{ image, manifest, "notes.txt" });
			const auto reused = cache.Request(MakeContentThumbnailRequest(editor, cache, material));
			REQUIRE(reused);
			CHECK(reused->Cached);
			CHECK(reused->Path == second->Path);
		}

		TEST_CASE("ThumbnailCache: a first thumbnail of an unloaded asset renders once at its loaded version")
		{
			Test::EditorTestFixture fixture("ThumbnailFirstLoad");
			fixture.CreateAndOpenProject();
			auto& editor = fixture.GetEditor();
			const AssetHandle material = WriteThumbnailMaterial(editor, "Assets/Unloaded.material", 0.5f);
			ReopenThumbnailProject(fixture);
			REQUIRE(editor.GetAssets().GetState(material) == AssetState::Unloaded);
			uint32_t renders = 0;
			// Like the production renderer, rendering loads the asset, which publishes a new version of an unloaded asset.
			ThumbnailCache cache(editor, [&renders, &editor](const ThumbnailRequest& request) -> Result<Image>
			{
				++renders;
				ENGINE_TRY(editor.GetAssets().Load(request.Asset));
				return RenderContentTestThumbnail(request);
			});
			REQUIRE(cache.BindProject(1));
			const auto unloaded = MakeContentThumbnailRequest(editor, cache, material);
			REQUIRE(cache.Queue(unloaded));
			REQUIRE(cache.Pump());
			CHECK(renders == 0); // the load starts first; nothing is rendered for a version about to change
			editor.GetAssets().WaitIdle();
			const auto loaded = MakeContentThumbnailRequest(editor, cache, material);
			REQUIRE(loaded.Version > unloaded.Version); // the load published a new version
			CHECK_FALSE(cache.Find(loaded)->has_value());
			REQUIRE(cache.Queue(loaded));
			REQUIRE(cache.Pump());
			CHECK(renders == 1);
			const auto found = cache.Find(loaded);
			REQUIRE(found);
			REQUIRE(found->has_value());
			CHECK_FALSE((*found)->Stale);
			REQUIRE(cache.Pump());
			CHECK(renders == 1);
		}

		TEST_CASE("ThumbnailCache: a request whose asset is still loading waits without blocking the safe point")
		{
			Test::EditorTestFixture fixture("ThumbnailLoading");
			fixture.CreateAndOpenProject();
			auto& editor = fixture.GetEditor();
			const AssetHandle material = WriteThumbnailMaterial(editor, "Assets/Loading.material", 0.5f);
			ReopenThumbnailProject(fixture);
			uint32_t renders = 0;
			ThumbnailCache cache(editor, [&renders](const ThumbnailRequest& request)
			{
				++renders;
				return RenderContentTestThumbnail(request);
			});
			REQUIRE(cache.BindProject(1));
			// An import in flight: the inline job ran, but its result is published only by a main-thread drain.
			static_cast<void>(editor.GetAssets().LoadAsync(material));
			REQUIRE(editor.GetAssets().GetState(material) == AssetState::Loading);
			REQUIRE(cache.Queue(MakeContentThumbnailRequest(editor, cache, material)));
			for (int frame = 0; frame < 3; ++frame)
				REQUIRE(cache.Pump());
			CHECK(renders == 0);
			// Pump neither rendered nor waited for (drained) the pending publication.
			CHECK(editor.GetAssets().GetState(material) == AssetState::Loading);
			editor.GetAssets().WaitIdle();
			REQUIRE(cache.Pump());
			CHECK(renders == 1);
			const auto found = cache.Find(MakeContentThumbnailRequest(editor, cache, material));
			REQUIRE(found);
			CHECK(found->has_value());
		}

		TEST_CASE("ThumbnailCache: queued work for a deleted asset is dropped silently")
		{
			Test::EditorTestFixture fixture("ThumbnailDeleted");
			fixture.CreateAndOpenProject();
			auto& editor = fixture.GetEditor();
			const AssetHandle material = WriteThumbnailMaterial(editor, "Assets/Deleted.material", 0.5f);
			uint32_t renders = 0;
			ThumbnailCache cache(editor, [&renders](const ThumbnailRequest& request)
			{
				++renders;
				return RenderContentTestThumbnail(request);
			});
			REQUIRE(cache.BindProject(1));
			REQUIRE(cache.Queue(MakeContentThumbnailRequest(editor, cache, material)));
			const auto source = editor.GetProject().GetRoot() / "Assets/Deleted.material";
			REQUIRE(FileSystem::Remove(source));
			REQUIRE(FileSystem::Remove(source.parent_path() / "Deleted.material.meta"));
			REQUIRE(editor.GetAssets().Refresh());
			REQUIRE(editor.GetAssets().GetAssetType(material) == AssetType::None);
			const Status pumped = cache.Pump();
			CHECK_MESSAGE(pumped.has_value(), (pumped ? std::string() : pumped.error().ToString()));
			CHECK(renders == 0);
			CHECK(cache.Pump().has_value());
		}

		TEST_CASE("ThumbnailCache: a referenced texture change marks a material image stale and renders it again")
		{
			Test::EditorTestFixture fixture("ThumbnailDependency");
			fixture.CreateAndOpenProject();
			auto& editor = fixture.GetEditor();
			const AssetHandle texture = WriteThumbnailTexture(editor, "Assets/Textures/Base.png", 16, std::byte{ 40 });
			const AssetHandle material = WriteThumbnailMaterial(editor, "Assets/Textured.material", 0.5f, texture);
			uint32_t renders = 0;
			ThumbnailCache cache(editor, [&renders](const ThumbnailRequest& request)
			{
				++renders;
				return RenderContentTestThumbnail(request);
			});
			REQUIRE(cache.BindProject(1));
			REQUIRE(cache.Queue(MakeContentThumbnailRequest(editor, cache, material)));
			for (int frame = 0; frame < 4 && renders == 0; ++frame)
			{
				REQUIRE(cache.Pump());
				editor.GetAssets().WaitIdle();
			}
			REQUIRE(renders == 1);
			const auto request = MakeContentThumbnailRequest(editor, cache, material);
			const auto before = cache.Find(request);
			REQUIRE(before);
			REQUIRE(before->has_value());
			CHECK_FALSE((*before)->Stale);

			const uint64_t textureVersion = editor.GetAssets().GetVersion(texture);
			REQUIRE(WriteThumbnailTexture(editor, "Assets/Textures/Base.png", 32, std::byte{ 200 }) == texture);
			REQUIRE(editor.GetAssets().GetVersion(texture) > textureVersion);
			// The material's own cooked bytes are unchanged, so its version is too: only its input changed.
			REQUIRE(editor.GetAssets().GetVersion(material) == request.Version);
			const auto stale = cache.Find(request);
			REQUIRE(stale);
			REQUIRE(stale->has_value());
			CHECK((*stale)->Stale);
			CHECK((*stale)->Path == (*before)->Path); // still displayable until regenerated
			REQUIRE(cache.Queue(request));
			REQUIRE(cache.Pump());
			CHECK(renders == 2);
			const auto after = cache.Find(request);
			REQUIRE(after);
			REQUIRE(after->has_value());
			CHECK_FALSE((*after)->Stale);
			CHECK((*after)->Path != (*before)->Path);
		}

		TEST_CASE("ThumbnailCache: a rendering quality change marks captured images stale and keys their disk entries")
		{
			Test::AutomationFixture fixture("ThumbnailQuality", false);
			auto& editor = fixture.GetEditor();
			const AssetHandle material = WriteThumbnailMaterial(editor, "Assets/Quality.material", 0.5f);
			const AssetHandle texture = WriteThumbnailTexture(editor, "Assets/Textures/Quality.png", 16, std::byte{ 90 });
			std::vector<AssetHandle> rendered;
			ThumbnailCache cache(editor, [&rendered](const ThumbnailRequest& request)
			{
				rendered.push_back(request.Asset);
				return RenderContentTestThumbnail(request);
			});
			REQUIRE(cache.BindProject(1));
			const RenderingSettings original = editor.GetProject().GetSettings().Rendering;
			CHECK(cache.GetRenderQuality().ShadowMapSize == original.ShadowMapSize);
			CHECK(cache.GetRenderQuality().SsaoHalfResolution == original.SsaoHalfResolution);
			const auto first = cache.Request(MakeContentThumbnailRequest(editor, cache, material));
			REQUIRE(first);
			REQUIRE(cache.Request(MakeContentThumbnailRequest(editor, cache, texture)));
			REQUIRE(rendered.size() == 2);

			const auto setQuality = [&fixture](uint32_t shadowMapSize, bool halfResolution)
			{
				REQUIRE(fixture.Call("project.setSettings", Json{ { "patch", Json{ { "Rendering", Json{ { "ShadowMapSize", shadowMapSize }, { "SsaoHalfResolution", halfResolution } } } } } }));
			};
			setQuality(original.ShadowMapSize == 512 ? 1024 : 512, !original.SsaoHalfResolution);
			CHECK(cache.GetRenderQuality().ShadowMapSize == editor.GetProject().GetSettings().Rendering.ShadowMapSize);
			CHECK(cache.GetRenderQuality().SsaoHalfResolution == !original.SsaoHalfResolution);
			// The captured material image stays displayable until regenerated; a texture's decoded preview has no quality.
			const auto stale = cache.Find(MakeContentThumbnailRequest(editor, cache, material));
			REQUIRE(stale);
			REQUIRE(stale->has_value());
			CHECK((*stale)->Stale);
			CHECK((*stale)->Path == first->Path);
			const auto textureImage = cache.Find(MakeContentThumbnailRequest(editor, cache, texture));
			REQUIRE(textureImage);
			REQUIRE(textureImage->has_value());
			CHECK_FALSE((*textureImage)->Stale);
			const auto second = cache.Request(MakeContentThumbnailRequest(editor, cache, material));
			REQUIRE(second);
			CHECK_FALSE(second->Cached);
			CHECK(second->Path != first->Path);
			CHECK(cache.Request(MakeContentThumbnailRequest(editor, cache, texture))->Cached);
			CHECK(rendered.size() == 3);

			// A later binding (another session) reuses the image of the current quality, never the earlier one's.
			cache.Reset();
			REQUIRE(cache.BindProject(2));
			const auto reused = cache.Request(MakeContentThumbnailRequest(editor, cache, material));
			REQUIRE(reused);
			CHECK(reused->Cached);
			CHECK(reused->Path == second->Path);
			CHECK_FALSE(FileSystem::Exists(FileSystem::PathFromUtf8(first->Path)));
			setQuality(original.ShadowMapSize, original.SsaoHalfResolution);
			cache.Reset();
			REQUIRE(cache.BindProject(3));
			const auto restored = cache.Request(MakeContentThumbnailRequest(editor, cache, material));
			REQUIRE(restored);
			CHECK_FALSE(restored->Cached);
			CHECK(rendered.size() == 4);
		}

		TEST_CASE("ThumbnailCache: a remembered failure is retried after a referenced asset changes")
		{
			Test::EditorTestFixture fixture("ThumbnailRetry");
			fixture.CreateAndOpenProject();
			auto& editor = fixture.GetEditor();
			const AssetHandle texture = WriteThumbnailTexture(editor, "Assets/Textures/Retry.png", 16, std::byte{ 40 });
			const AssetHandle material = WriteThumbnailMaterial(editor, "Assets/Retry.material", 0.5f, texture);
			bool fail = true;
			uint32_t renders = 0;
			ThumbnailCache cache(editor, [&renders, &fail](const ThumbnailRequest& request) -> Result<Image>
			{
				++renders;
				if (fail)
					return MakeError(ErrorCode::Io, "injected dependency failure");
				return RenderContentTestThumbnail(request);
			});
			REQUIRE(cache.BindProject(1));
			const auto request = MakeContentThumbnailRequest(editor, cache, material);
			CHECK(cache.Request(request).error().GetCode() == ErrorCode::Io);
			CHECK(cache.Find(request).error().GetCode() == ErrorCode::Io);
			REQUIRE(cache.Queue(request));
			REQUIRE(cache.Pump());
			CHECK(renders == 1); // remembered while its inputs are unchanged

			fail = false;
			REQUIRE(WriteThumbnailTexture(editor, "Assets/Textures/Retry.png", 32, std::byte{ 90 }) == texture);
			REQUIRE(editor.GetAssets().GetVersion(material) == request.Version);
			const auto changed = cache.Find(request);
			REQUIRE(changed);
			CHECK_FALSE(changed->has_value());
			REQUIRE(cache.Queue(request));
			REQUIRE(cache.Pump());
			CHECK(renders == 2);
			const auto found = cache.Find(request);
			REQUIRE(found);
			CHECK(found->has_value());
		}

		TEST_CASE("ThumbnailCache: revalidation catches a source changed while its asset was not loaded")
		{
			Test::EditorTestFixture fixture("ThumbnailRevalidate");
			fixture.CreateAndOpenProject();
			auto& editor = fixture.GetEditor();
			const AssetHandle material = WriteThumbnailMaterial(editor, "Assets/Outside.material", 0.25f);
			uint32_t renders = 0;
			ThumbnailCache cache(editor, [&renders](const ThumbnailRequest& request)
			{
				++renders;
				return RenderContentTestThumbnail(request);
			});
			REQUIRE(cache.BindProject(1));
			const auto first = cache.Request(MakeContentThumbnailRequest(editor, cache, material));
			REQUIRE(first);
			cache.Reset();
			ReopenThumbnailProject(fixture);
			REQUIRE(cache.BindProject(2));
			const auto request = MakeContentThumbnailRequest(editor, cache, material);
			REQUIRE(cache.Request(request)->Cached);
			// Edited outside the editor while unloaded: no reload, no version change, nothing in memory notices.
			MaterialData edited;
			edited.Roughness = 0.987654f;
			const auto text = MaterialToText(edited, editor.GetTypeRegistry());
			REQUIRE(text);
			REQUIRE(FileSystem::WriteFileAtomic(editor.GetProject().GetRoot() / "Assets/Outside.material", AsBytes(*text)));
			CHECK_FALSE((*cache.Find(request))->Stale);
			cache.Revalidate();
			const auto stale = cache.Find(request);
			REQUIRE(stale);
			REQUIRE(stale->has_value());
			CHECK((*stale)->Stale);
			REQUIRE(cache.Queue(request));
			for (int frame = 0; frame < 4 && renders == 1; ++frame)
			{
				REQUIRE(cache.Pump());
				editor.GetAssets().WaitIdle();
				if (const auto current = MakeContentThumbnailRequest(editor, cache, material); current.Version != request.Version)
					REQUIRE(cache.Queue(current));
			}
			CHECK(renders == 2);
		}

		TEST_CASE("ThumbnailCache: a changed bound project rejects queued work before rendering")
		{
			Test::EditorTestFixture fixture("ThumbnailWrongProject");
			fixture.CreateAndOpenProject("First");
			uint32_t calls = 0;
			ThumbnailCache cache(fixture.GetEditor(), [&calls](const ThumbnailRequest& request)
			{
				++calls;
				return RenderContentTestThumbnail(request);
			});
			REQUIRE(cache.BindProject(1));
			const auto request = MakeContentThumbnailRequest(fixture.GetEditor(), cache);
			REQUIRE(cache.Queue(request));
			// Simulate a broken host forgetting Reset; even then work must never cross into a different project.
			REQUIRE(fixture.GetEditor().CloseProject());
			fixture.CreateAndOpenProject("Second");
			CHECK(cache.Pump().error().GetCode() == ErrorCode::Conflict);
			CHECK(cache.Request(request).error().GetCode() == ErrorCode::Conflict);
			CHECK(cache.Find(request).error().GetCode() == ErrorCode::Conflict);
			CHECK(calls == 0);
		}

		TEST_CASE("ThumbnailCache: malformed renderer output is remembered without publishing a PNG")
		{
			Test::EditorTestFixture fixture("ThumbnailBadImage");
			fixture.CreateAndOpenProject();
			uint32_t calls = 0;
			ThumbnailCache cache(fixture.GetEditor(), [&calls](const ThumbnailRequest&) -> Result<Image>
			{
				++calls;
				return Image{};
			});
			REQUIRE(cache.BindProject(1));
			const auto request = MakeContentThumbnailRequest(fixture.GetEditor(), cache);
			CHECK(cache.Request(request).error().GetCode() == ErrorCode::Validation);
			CHECK(cache.Find(request).error().GetCode() == ErrorCode::Validation);
			REQUIRE(cache.Queue(request));
			REQUIRE(cache.Pump());
			CHECK(calls == 1);
			const auto entries = FileSystem::ListDirectory(fixture.GetEditor().GetProject().GetCacheDirectory(), true);
			REQUIRE(entries);
			for (const auto& path : *entries)
				CHECK(path.extension() != ".png");
		}

		TEST_CASE("ThumbnailCache: queue is memory only deduplicated and bounded")
		{
			Test::EditorTestFixture fixture("ThumbnailQueue");
			fixture.CreateAndOpenProject();
			auto& editor = fixture.GetEditor();
			// Loaded inputs render at the first visit; unloaded ones would start their loads first.
			REQUIRE(editor.GetAssets().Load(BuiltinAssetHandles::CubeMesh));
			REQUIRE(editor.GetAssets().Load(BuiltinAssetHandles::SphereMesh));
			uint32_t calls = 0;
			ThumbnailCache cache(editor, [&calls](const ThumbnailRequest& request)
			{
				++calls;
				return RenderContentTestThumbnail(request);
			});
			REQUIRE(cache.BindProject(1));
			const auto first = MakeContentThumbnailRequest(editor, cache);
			const auto second = MakeContentThumbnailRequest(editor, cache, BuiltinAssetHandles::SphereMesh);
			REQUIRE(cache.Queue(first));
			REQUIRE(cache.Queue(first));
			REQUIRE(cache.Queue(second));
			CHECK_FALSE(cache.Find(first)->has_value());
			CHECK(calls == 0);
			CHECK(cache.Pump(0).error().GetCode() == ErrorCode::InvalidArgument);
			REQUIRE(cache.Pump(1));
			CHECK(cache.Find(first)->has_value());
			CHECK_FALSE(cache.Find(second)->has_value());
			CHECK(calls == 1);
			cache.Invalidate(second.Asset);
			REQUIRE(cache.Pump());
			CHECK(calls == 1);
		}
	}

}
