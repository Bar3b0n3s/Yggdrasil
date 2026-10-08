#pragma once

#include "EditorCore/Automation/AutomationTypes.h"
#include "Engine/Automation/Methods/AutomationMethodContext.h"
#include "Engine/Automation/Protocol/MethodContext.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Core/VfsPath.h"

#include <chrono>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace Engine {

	class AutomationServer;
	class ConstEntity;
	class EditorContext;
	class Entity;
	class Scene;

	// The context every editor method handler receives (MethodRegistry: Host = EditorMethodContext). It gives handlers the
	// editor state and the shared resolution rules of §13.4, so every domain resolves entity references, paths and targets
	// the same way. Since M7 it derives from AutomationMethodContext (Engine/Automation/Methods), so the handlers the Editor
	// and the Runtime share run on it too; it implements the shared services over the EditorContext and its server
	// (Docs/Decisions/0012-m7-decisions.md decision 12). One per request; main thread only.
	class EditorMethodContext final : public AutomationMethodContext
	{
	public:
		// `editor` and `server` are documented back-references that outlive the request.
		EditorMethodContext(EditorContext& editor, AutomationServer& server, MethodRequest request);

		[[nodiscard]] EditorContext& GetEditor() const { return *m_Editor; }
		[[nodiscard]] AutomationServer& GetServer() const { return *m_Server; }

		// A context for one op of edit.batch, over the same editor and server.
		[[nodiscard]] Scope<MethodContext> CreateNested(MethodRequest request) const override;
		// The server's resolver (AutomationServer::GetAssetReferenceResolver): requests and edit.batch's ops resolve asset references
		// in their params through it (MethodRegistry.h convention 13).
		[[nodiscard]] IAssetReferenceResolver* GetAssetReferenceResolver() const override;

		// The scene a request addresses (§13.4 "Target"): `target` as given when `given`, else the play scene for reads while
		// playing (EditorPlayController) and the edit scene otherwise. Errors: InvalidState "no scene open" without an open
		// edit scene, InvalidState "not playing" for the play scene without a session (located at /target). The pointer is
		// non-owning and valid until the open scene or the play session changes.
		[[nodiscard]] Result<Scene*> ResolveTargetScene(SceneTarget target, bool given, bool mutation) const override;

		// Resolves an EntityRef (§13.4) in `scene`: 16 hex digits (either case) is an exact id; 6 to 15 hex digits a unique
		// prefix of an entity's id; text starting with '/' an entity path (Scene::ResolveEntityPath). `pointer` locates the
		// param in errors ("/entity", "/entities/2"). Errors: InvalidArgument for anything else, or for an ambiguous prefix or
		// path (every candidate listed as an ErrorIssue with its path); NotFound for a valid reference that names no entity
		// (with "did you mean" suggestions for paths).
		[[nodiscard]] Result<Entity> ResolveEntity(Scene& scene, std::string_view reference, std::string_view pointer) const override;

		// Resolves a project path param (§13.2 "Paths"): "Assets/Scenes/Main.scene" (project-relative) or
		// "project://Assets/Scenes/Main.scene". With a non-empty `extension` (".scene") the path must end with it (ASCII
		// case-sensitive). `pointer` locates the param in errors. Errors: InvalidState without an open project;
		// InvalidArgument for another scheme, an absolute or escaping path, NUL or reserved names (VfsPath rules), an empty
		// path or a wrong extension.
		[[nodiscard]] Result<VfsPath> ResolveProjectPath(std::string_view path, std::string_view pointer, std::string_view extension = {}) const;

		// {id, name, path} of `entity` (valid, asserted).
		[[nodiscard]] EntitySummary MakeEntitySummary(ConstEntity entity) const override;

		// The shared services of AutomationMethodContext over the editor: the play controller's session, StartPlay and StopPlay
		// (EditorContext::GetPlay), the server's client names, the engine's event log, the server's view capture
		// (ScreenshotCaptures::View; Unsupported without a device), the editor's scene-view camera
		// (EditorContext::GetSceneViewCamera) and output files (AutomationServer::WriteOutputFile).
		[[nodiscard]] PlaySession* GetPlaySession() const override;
		[[nodiscard]] Status StartPlay(const PlayStartOptions& options) override;
		[[nodiscard]] Status StopPlay() override;
		[[nodiscard]] std::string GetClientName(ClientId client) const override;
		[[nodiscard]] EventLog& GetEventLog() const override;
		[[nodiscard]] Result<Image> CaptureView(const RenderSnapshot& snapshot, const ViewportScreenshotRequest& request) override;
		[[nodiscard]] std::optional<ExplicitRenderCamera> GetSceneViewCamera() const override;
		[[nodiscard]] Result<std::string> WriteOutputFile(std::string_view extension, std::span<const std::byte> bytes) override;

		// The services of the moved M4 to M6 domains (Docs/Decisions/0012-m7-decisions.md decision 12) over the editor:
		// DescribeSession reports the server's capabilities, clients, renderer and headless flag and the open project;
		// Shutdown saves the open scene with save (InvalidState for a dirty scene without save or force, PermissionDenied for
		// save in a read-only editor) and requests the exit (EditorContext::RequestShutdown); MakeSceneSummary reports the
		// edit scene's file and dirty flag (Utils::MakeSceneSummary), and for the play scene the edit scene's file, never
		// dirty; GetAssets is the EditorAssetManager; GetWallClockTime is the server's AutomationServerSpecification::WallClock.
		[[nodiscard]] SessionHostDescription DescribeSession() const override;
		[[nodiscard]] Result<SessionShutdownResult> Shutdown(const SessionShutdownParams& params) override;
		[[nodiscard]] SceneSummary MakeSceneSummary(const Scene& scene) const override;
		[[nodiscard]] AssetManager* GetAssets() const override;
		[[nodiscard]] std::chrono::steady_clock::time_point GetWallClockTime() const override;
	private:
		EditorContext* m_Editor = nullptr;    // documented back-reference
		AutomationServer* m_Server = nullptr; // documented back-reference
	};

}
