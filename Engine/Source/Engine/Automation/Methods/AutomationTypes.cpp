#include "EnginePCH.h"
#include "Engine/Automation/Methods/AutomationTypes.h"

#include "Engine/Core/EventLog.h"
#include "Engine/Core/RingBufferSink.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Reflection/ValidationContext.h"

namespace Engine {

	void RegisterAutomationSharedTypes(TypeRegistry& registry)
	{
		registry.Enum<SceneTarget>("SceneTarget", "Which scene a request reads or changes (§13.4 \"Target\").")
			.Entry(SceneTarget::Edit, "Edit", "The scene open in the editor; changes are undoable commands.")
			.Entry(SceneTarget::Play, "Play", "The running play session's scene; changes are transient and never undone.");

		registry.Enum<DiagnosticSeverity>("DiagnosticSeverity", "How serious a diagnostic is.")
			.Entry(DiagnosticSeverity::Warning, "Warning", "Worth fixing; nothing is blocked by it.")
			.Entry(DiagnosticSeverity::Error, "Error", "Must be fixed; it can block play or export.");

		registry.Enum<LogLevel>("LogLevel", "The level of a log entry.")
			.Entry(LogLevel::Trace, "Trace", "Detailed tracing, compiled out in Dist.")
			.Entry(LogLevel::Info, "Info", "Normal operation.")
			.Entry(LogLevel::Warn, "Warn", "Something unexpected that the engine handled.")
			.Entry(LogLevel::Error, "Error", "An operation failed.")
			.Entry(LogLevel::Critical, "Critical", "The process cannot continue normally.");

		registry.Enum<LogChannel>("LogChannel", "The logger an entry came from.")
			.Entry(LogChannel::Engine, "Engine", "The engine's own messages.")
			.Entry(LogChannel::App, "App", "The editor's or the game executable's messages.")
			.Entry(LogChannel::Script, "Script", "Luau Log.* calls and print.");

		registry.Enum<EngineEventType>("EngineEventType", "The kind of an engine event (§4.9).")
			.Entry(EngineEventType::EntityCreated, "EntityCreated", "An entity was created.")
			.Entry(EngineEventType::EntityDestroyed, "EntityDestroyed", "An entity was destroyed.")
			.Entry(EngineEventType::ComponentChanged, "ComponentChanged", "A component of an entity changed.")
			.Entry(EngineEventType::SceneOpened, "SceneOpened", "A scene became the open scene.")
			.Entry(EngineEventType::SceneChangedOnDisk, "SceneChangedOnDisk", "The open scene's file, or a prefab it uses, changed on disk.")
			.Entry(EngineEventType::PlayStateChanged, "PlayStateChanged", "Play mode started, paused, resumed or stopped.")
			.Entry(EngineEventType::AssetReloaded, "AssetReloaded", "An asset was reloaded after a change.")
			.Entry(EngineEventType::AssetImportFailed, "AssetImportFailed", "Importing an asset failed.")
			.Entry(EngineEventType::ScriptErrorRaised, "ScriptErrorRaised", "A script raised an error.")
			.Entry(EngineEventType::DiagnosticsChanged, "DiagnosticsChanged", "The project's diagnostics changed.")
			.Entry(EngineEventType::AutomationClientDisconnected, "AutomationClientDisconnected", "An automation client disconnected.");

		registry.Struct<EntitySummary>("EntitySummary", "An entity's canonical id with its readable name and path.")
			.Field("id", &EntitySummary::Id, "The entity's id: 16 lowercase hex digits.")
			.Field("name", &EntitySummary::Name, "The entity's name.")
			.Field("path", &EntitySummary::Path, "The entity's path from the scene root, such as \"/Game/Board\".");

		registry.Enum<AssetType>("AssetType", "The kind of a loadable asset (§7.1).")
			.Entry(AssetType::None, "None", "No type; as a filter, every type.")
			.Entry(AssetType::Scene, "Scene", "A scene (.scene).")
			.Entry(AssetType::Prefab, "Prefab", "A prefab (.prefab, or the hierarchy of a glTF model).")
			.Entry(AssetType::Mesh, "Mesh", "A mesh (a glTF's mesh sub-asset, or a built-in mesh).")
			.Entry(AssetType::Material, "Material", "A PBR material (.material, or a glTF's material sub-asset).")
			.Entry(AssetType::Texture, "Texture", "A texture (PNG, JPEG, TGA, BMP, or a glTF's texture sub-asset).")
			.Entry(AssetType::Environment, "Environment", "An image-based lighting environment (.hdr).")
			.Entry(AssetType::AudioClip, "AudioClip", "A sound (WAV, FLAC, MP3, or a .sfx preset).")
			.Entry(AssetType::Script, "Script", "A Luau script (.luau).")
			.Entry(AssetType::Font, "Font", "A font (TTF, OTF) as a signed distance field atlas.")
			.Entry(AssetType::Replay, "Replay", "An input replay (.replay).");

		registry.Struct<AssetSummary>("AssetSummary", "An asset's canonical id with its readable path and type.")
			.Field("id", &AssetSummary::Id, "The asset's handle: 16 lowercase hex digits.")
			.Field("path", &AssetSummary::Path, "Its readable reference, such as \"Assets/Models/Track.glb#mesh:0:Straight\" or \"engine://Meshes/Cube\".")
			.Field("type", &AssetSummary::Type, "Its type.");

		static_cast<void>(registry.Struct<NoParams>("NoParams", "The params of a method that takes none: an empty object."));
	}

}
