#pragma once

#include "EditorCore/Automation/AutomationTypes.h"
#include "Engine/Automation/Protocol/MethodContext.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Core/VfsPath.h"

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
	// the same way. One per request; main thread only.
	class EditorMethodContext final : public MethodContext
	{
	public:
		// `editor` and `server` are documented back-references that outlive the request.
		EditorMethodContext(EditorContext& editor, AutomationServer& server, MethodRequest request);

		[[nodiscard]] EditorContext& GetEditor() const { return *m_Editor; }
		[[nodiscard]] AutomationServer& GetServer() const { return *m_Server; }

		// A context for one op of edit.batch, over the same editor and server.
		[[nodiscard]] Scope<MethodContext> CreateNested(MethodRequest request) const override;

		// The scene a request addresses (§13.4 "Target"): `target` as given when `given`, else the play scene for reads while
		// playing and the edit scene otherwise. Errors: InvalidState "no scene open" without an open edit scene, InvalidState
		// "not playing" for the play scene (always in M4, which has no play sessions). The pointer is non-owning and valid
		// until the open scene changes.
		[[nodiscard]] Result<Scene*> ResolveTargetScene(SceneTarget target, bool given, bool mutation) const;

		// Resolves an EntityRef (§13.4) in `scene`: 16 hex digits (either case) is an exact id; 6 to 15 hex digits a unique
		// prefix of an entity's id; text starting with '/' an entity path (Scene::ResolveEntityPath). `pointer` locates the
		// param in errors ("/entity", "/entities/2"). Errors: InvalidArgument for anything else, or for an ambiguous prefix or
		// path (every candidate listed as an ErrorIssue with its path); NotFound for a valid reference that names no entity
		// (with "did you mean" suggestions for paths).
		[[nodiscard]] Result<Entity> ResolveEntity(Scene& scene, std::string_view reference, std::string_view pointer) const;

		// Resolves a project path param (§13.2 "Paths"): "Assets/Scenes/Main.scene" (project-relative) or
		// "project://Assets/Scenes/Main.scene". With a non-empty `extension` (".scene") the path must end with it (ASCII
		// case-sensitive). `pointer` locates the param in errors. Errors: InvalidState without an open project;
		// InvalidArgument for another scheme, an absolute or escaping path, NUL or reserved names (VfsPath rules), an empty
		// path or a wrong extension.
		[[nodiscard]] Result<VfsPath> ResolveProjectPath(std::string_view path, std::string_view pointer, std::string_view extension = {}) const;

		// {id, name, path} of `entity` (valid, asserted).
		[[nodiscard]] EntitySummary MakeEntitySummary(ConstEntity entity) const;
	private:
		EditorContext* m_Editor = nullptr;    // documented back-reference
		AutomationServer* m_Server = nullptr; // documented back-reference
	};

}
