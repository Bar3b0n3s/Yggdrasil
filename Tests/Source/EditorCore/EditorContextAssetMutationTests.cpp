#include "TestsPCH.h"

#include "EditorCore/EditorContext.h"

#include "EditorCore/EditorActions.h"
#include "Engine/AssetPipeline/EditorAssetManager.h"
#include "Engine/Core/Buffer.h"
#include "Engine/Core/VirtualFileSystem.h"
#include "Support/AutomationTestClient.h"

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <map>
#include <string>
#include <string_view>

namespace Engine {

	namespace {

		static VfsPath AssetPolicyPath(std::string_view path)
		{
			const auto parsed = VfsPath::Create("project", path);
			REQUIRE(parsed);
			return *parsed;
		}

		// Include source files, existing metadata, backups and provenance in an independent byte-for-byte disk oracle.
		static std::map<std::string, Buffer> CaptureAssetPolicyFiles(const VirtualFileSystem& vfs)
		{
			const auto entries = vfs.List(AssetPolicyPath(""), true);
			REQUIRE(entries);
			std::map<std::string, Buffer> result;
			for (const VfsEntry& entry : *entries)
			{
				if (entry.Info.IsDirectory)
					continue;
				const auto bytes = vfs.ReadFile(entry.Path);
				REQUIRE(bytes);
				result.emplace(entry.Path.ToString(), *bytes);
			}
			return result;
		}

		static VfsPath AddAssetPolicySource(EditorContext& editor)
		{
			editor.GetAssets().WaitIdle();
			const VfsPath source = AssetPolicyPath("Assets/External.material");
			// An external editor's new source: deliberately bypass the editor's asset writer, so it has no metadata yet.
			REQUIRE(editor.GetVfs().WriteFileAtomic(source, AsBytes(R"({"Format":"Material","Version":1,"Roughness":0.3})")));
			return source;
		}

	}

	TEST_SUITE("EditorCore")
	{
		TEST_CASE("EditorContext: denied agent asset refresh and implicit resolution leave project bytes unchanged")
		{
			for (const bool implicit : { false, true })
			{
				INFO("implicit path resolution ", implicit);
				Test::AutomationFixture fixture("AssetPolicyDenied");
				EditorContext& editor = fixture.GetEditor();
				const VfsPath source = AddAssetPolicySource(editor);
				const VfsPath meta = AssetPolicyPath("Assets/External.material.meta");
				const auto before = CaptureAssetPolicyFiles(editor.GetVfs());
				fixture.GetClient().GetServer().SetEditorPolicy({ .DenyMutations = true });
				const auto denied = implicit
					? fixture.Call("asset.info", Json{ { "asset", std::string(source.GetPath()) } })
					: fixture.Call("project.refreshAssets", Json::object());
				CHECK_FALSE(denied);
				if (!denied)
					CHECK(denied.error().GetCode() == ErrorCode::PermissionDenied);
				CHECK_FALSE(editor.GetVfs().Exists(meta));
				CHECK((CaptureAssetPolicyFiles(editor.GetVfs()) == before));
				CHECK_FALSE(editor.GetAssets().Resolve(source.GetPath()));
				CHECK(editor.GetCommandOrigin() == CommandOrigin::User);
			}
		}

		TEST_CASE("EditorContext: asset writer policy permits queued human and passive refresh")
		{
			for (const bool humanAction : { false, true })
			{
				INFO("queued human action ", humanAction);
				Test::AutomationFixture fixture("AssetPolicyHuman");
				EditorContext& editor = fixture.GetEditor();
				const VfsPath source = AddAssetPolicySource(editor);
				const auto bytes = editor.GetVfs().ReadFile(source);
				REQUIRE(bytes);
				fixture.GetClient().GetServer().SetEditorPolicy({ .DenyMutations = true });
				if (humanAction)
				{
					EditorActions actions(editor, fixture.GetClient().GetServer());
					const auto ticket = actions.Submit("project.refreshAssets", Json::object());
					REQUIRE(ticket);
					actions.Pump();
					const auto result = actions.TakeResult(*ticket);
					REQUIRE(result);
					REQUIRE(result->has_value());
					REQUIRE(**result);
				}
				else
					REQUIRE(editor.GetAssets().Refresh());
				CHECK(editor.GetVfs().Exists(AssetPolicyPath("Assets/External.material.meta")));
				CHECK(editor.GetAssets().Resolve(source.GetPath()));
				const auto after = editor.GetVfs().ReadFile(source);
				REQUIRE(after);
				CHECK(*after == *bytes);
				CHECK(editor.AreAgentMutationsDenied());
				// Once no metadata write is needed, ordinary agent inspection stays available.
				CHECK(fixture.Call("asset.info", Json{ { "asset", std::string(source.GetPath()) } }));
			}
		}

		TEST_CASE("EditorContext: asset writer policy allows dry-run resolution and restores successful and failed overlays")
		{
			Test::AutomationFixture fixture("AssetPolicyDryRun");
			EditorContext& editor = fixture.GetEditor();
			const VfsPath source = AddAssetPolicySource(editor);
			const auto before = CaptureAssetPolicyFiles(editor.GetVfs());
			const uint64_t revision = editor.GetRevision();
			fixture.GetClient().GetServer().SetEditorPolicy({ .DenyMutations = true });
			for (const bool valid : { true, false })
			{
				INFO("valid asset patch ", valid);
				const auto result = fixture.Call("asset.setProperties", Json{ { "asset", std::string(source.GetPath()) }, { "values", Json{ { "Roughness", valid ? Json(0.7f) : Json("invalid") } } }, { "dryRun", true } });
				if (valid)
				{
					REQUIRE_MESSAGE(result.has_value(), (result ? "" : result.error().ToString()));
					CHECK((*result)["dryRun"] == Json(true));
					CHECK((*result)["values"]["Roughness"] == Json(0.7f));
				}
				else
				{
					REQUIRE_FALSE(result);
					CHECK(result.error().GetCode() == ErrorCode::Validation);
				}
				CHECK_FALSE(editor.IsDryRun());
				CHECK(editor.GetRevision() == revision);
				CHECK_FALSE(editor.GetAssets().Resolve(source.GetPath()));
				CHECK((CaptureAssetPolicyFiles(editor.GetVfs()) == before));
				const auto denied = fixture.Call("project.refreshAssets", Json::object());
				REQUIRE_FALSE(denied);
				CHECK(denied.error().GetCode() == ErrorCode::PermissionDenied);
				CHECK((CaptureAssetPolicyFiles(editor.GetVfs()) == before));
			}
		}
	}

}
