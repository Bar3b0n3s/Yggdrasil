#pragma once

#include "Engine/Automation/Methods/AutomationTypes.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Reflection/Value.h"

#include <cstdint>
#include <string>

// M28 (Docs/Decisions/0043-m28-game-services.md decision 8): editor play's save data through automation, the methods behind
// the Save Data panel: savedata.get reads the play store (Library/SaveData/Store.json, a read-only editor's private store)
// or the running session's, savedata.set seeds the play store and savedata.clear empties it. None changes project content,
// so a read-only editor serves them all, on its private store.

namespace Engine {

	class EditorMethodContext;
	class MethodRegistry;
	class TypeRegistry;

	// Which store savedata.get reads.
	enum class SaveDataTarget : uint8_t
	{
		Editor, // the play store, which a live play session starts with and its SaveData.Commit writes
		Play    // the running play session's store (any session: live, lockstep, a recording or a replay)
	};

	// savedata.get {target?}
	struct SaveDataGetParams
	{
		SaveDataTarget Target = SaveDataTarget::Editor;
	};

	struct SaveDataGetResult
	{
		VariantValue Values{};   // the store's values by key, an object
		uint32_t Count = 0;      // its keys
		uint32_t Bytes = 0;      // its compact JSON size, at most MaxSaveDataBytes
		std::string LoadError{}; // why the play store could not be read and was moved aside (empty when it could)
	};

	// savedata.set {values, merge?}
	struct SaveDataSetParams
	{
		VariantValue Values{};
		bool Merge = false;
	};

	// What savedata.set and savedata.clear left in the play store.
	struct SaveDataWriteResult
	{
		uint32_t Count = 0;
		uint32_t Bytes = 0;
		std::string LoadError{}; // savedata.set with merge: why its read moved the play store's file aside (empty when it could read it)
	};

	namespace Automation {

		[[nodiscard]] Result<SaveDataGetResult> SaveDataGet(EditorMethodContext& context, const SaveDataGetParams& params);
		[[nodiscard]] Result<SaveDataWriteResult> SaveDataSet(EditorMethodContext& context, const SaveDataSetParams& params);
		[[nodiscard]] Result<SaveDataWriteResult> SaveDataClear(EditorMethodContext& context, const NoParams& params);

	}

	void RegisterSaveDataMethodTypes(TypeRegistry& registry);
	void RegisterSaveDataMethods(MethodRegistry& methods);

}
