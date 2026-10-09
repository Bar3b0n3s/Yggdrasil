#include "TestsPCH.h"
#include "EditorCore/Thumbnails/ThumbnailCache.h"

#include "Engine/Asset/BuiltinAssets.h"
#include "Engine/AssetPipeline/EditorAssetManager.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Support/AutomationTestClient.h"

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
			CHECK(first->Path.contains("thumbnail-v1-"));
			CHECK(FileSystem::PathFromUtf8(first->Path).parent_path() == editor.GetProject().GetCacheDirectory());
			const auto png = ReadPng(FileSystem::PathFromUtf8(first->Path));
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
			CHECK(FileSystem::PathFromUtf8(result->Path).parent_path() == privateCache);
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
			const auto entries = FileSystem::ListDirectory(fixture.GetEditor().GetProject().GetCacheDirectory());
			REQUIRE(entries);
			for (const auto& path : *entries)
				CHECK_FALSE(FileSystem::PathToUtf8(path.filename()).starts_with("thumbnail-"));
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
			CHECK(FileSystem::PathFromUtf8(second->Path).parent_path() == editor.GetProject().GetCacheDirectory());
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

		TEST_CASE("ThumbnailCache: independent cache lifetimes never collide on disk")
		{
			Test::EditorTestFixture fixture("ThumbnailLifetimes");
			fixture.CreateAndOpenProject();
			ThumbnailCache first(fixture.GetEditor(), RenderContentTestThumbnail);
			ThumbnailCache second(fixture.GetEditor(), RenderContentTestThumbnail);
			REQUIRE(first.BindProject(1));
			REQUIRE(second.BindProject(1));
			const auto a = first.Request(MakeContentThumbnailRequest(fixture.GetEditor(), first));
			const auto b = second.Request(MakeContentThumbnailRequest(fixture.GetEditor(), second));
			REQUIRE(a);
			REQUIRE(b);
			CHECK(a->Path != b->Path);
			CHECK_FALSE(b->Cached);
			const auto aBytes = FileSystem::ReadFile(FileSystem::PathFromUtf8(a->Path));
			const auto bBytes = FileSystem::ReadFile(FileSystem::PathFromUtf8(b->Path));
			REQUIRE(aBytes);
			REQUIRE(bBytes);
			CHECK(*aBytes == *bBytes);
			const std::string filename = FileSystem::PathToUtf8(FileSystem::PathFromUtf8(a->Path).filename());
			CHECK(filename.find_first_not_of("0123456789abcdefghijklmnopqrstuvwxyz-.") == std::string::npos);
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
			const auto entries = FileSystem::ListDirectory(fixture.GetEditor().GetProject().GetCacheDirectory());
			REQUIRE(entries);
			for (const auto& path : *entries)
				CHECK(path.extension() != ".png");
		}

		TEST_CASE("ThumbnailCache: queue is memory only deduplicated and bounded")
		{
			Test::EditorTestFixture fixture("ThumbnailQueue");
			fixture.CreateAndOpenProject();
			uint32_t calls = 0;
			ThumbnailCache cache(fixture.GetEditor(), [&calls](const ThumbnailRequest& request)
			{
				++calls;
				return RenderContentTestThumbnail(request);
			});
			REQUIRE(cache.BindProject(1));
			const auto first = MakeContentThumbnailRequest(fixture.GetEditor(), cache);
			const auto second = MakeContentThumbnailRequest(fixture.GetEditor(), cache, BuiltinAssetHandles::SphereMesh);
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
