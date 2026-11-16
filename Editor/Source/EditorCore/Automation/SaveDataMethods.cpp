#include "EditorPCH.h"
#include "EditorCore/Automation/SaveDataMethods.h"

#include "EditorCore/Automation/EditorMethodContext.h"
#include "EditorCore/EditorContext.h"
#include "EditorCore/Play/EditorPlayController.h"
#include "Engine/Automation/Protocol/MethodRegistry.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scripting/ScriptLimits.h"
#include "Engine/Session/PlaySession.h"
#include "Engine/Session/SaveDataStore.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <format>
#include <limits>
#include <utility>

namespace Engine {

	namespace Utils {

		[[nodiscard]] static uint32_t ToSaveDataCount(size_t value)
		{
			return static_cast<uint32_t>(std::min<size_t>(value, std::numeric_limits<uint32_t>::max()));
		}

		[[nodiscard]] static Result<SaveDataWriteResult> WritePlayStore(EditorMethodContext& context, const SaveDataStore& store)
		{
			ENGINE_TRY(context.GetEditor().GetPlay().WritePlaySaveData(store.ToJson()));
			return SaveDataWriteResult{ .Count = ToSaveDataCount(store.GetSize()), .Bytes = ToSaveDataCount(store.GetSerializedBytes()) };
		}

	}

	namespace Automation {

		Result<SaveDataGetResult> SaveDataGet(EditorMethodContext& context, const SaveDataGetParams& params)
		{
			SaveDataStore store;
			SaveDataGetResult result;
			if (params.Target == SaveDataTarget::Play)
			{
				const PlaySession* session = context.GetPlaySession();
				if (session == nullptr)
					return std::unexpected(Error(ErrorCode::InvalidState, "no play session runs").WithHint("start one with play.start, or read the play store without target"));
				store = session->GetSaveData();
			}
			else
			{
				ENGINE_TRY_ASSIGN(const SaveDataFileLoad load, context.GetEditor().GetPlay().ReadPlaySaveData());
				ENGINE_TRY(store.Replace(load.Values.Get()));
				result.LoadError = load.Error.value_or(std::string());
			}
			result.Values = VariantValue(store.ToJson());
			result.Count = Utils::ToSaveDataCount(store.GetSize());
			result.Bytes = Utils::ToSaveDataCount(store.GetSerializedBytes());
			return result;
		}

		Result<SaveDataWriteResult> SaveDataSet(EditorMethodContext& context, const SaveDataSetParams& params)
		{
			if (!params.Values.Get().is_object())
				return MakeError(ErrorCode::InvalidArgument, "values must be an object of save-data values by key");
			SaveDataStore store;
			// A merge reads the play store first, and that read moves a store file it cannot read aside, so no later read finds
			// it again: why travels with this result, or with the error of a value the merge refuses or of a write that fails
			// (batch 2's merge review, finding 11).
			std::string loadError;
			if (params.Merge)
			{
				ENGINE_TRY_ASSIGN(const SaveDataFileLoad load, context.GetEditor().GetPlay().ReadPlaySaveData());
				ENGINE_TRY(store.Replace(load.Values.Get()));
				loadError = load.Error.value_or(std::string());
			}
			for (const auto& [key, value] : params.Values.Get().items())
			{
				if (Status set = WithContext(store.Set(key, value), std::format("at /values/{}", key)); !set)
				{
					if (loadError.empty())
						return std::unexpected(std::move(set).error());
					return std::unexpected(std::move(set).error().WithContext(std::format("the play store's file was read first: {}", loadError)));
				}
			}
			Result<SaveDataWriteResult> written = Utils::WritePlayStore(context, store);
			if (!written)
			{
				if (loadError.empty())
					return std::unexpected(std::move(written).error());
				return std::unexpected(std::move(written).error().WithContext(std::format("the play store's file was read first: {}", loadError)));
			}
			written->LoadError = std::move(loadError);
			return written;
		}

		Result<SaveDataWriteResult> SaveDataClear(EditorMethodContext& context, const NoParams& /*params*/)
		{
			return Utils::WritePlayStore(context, SaveDataStore{});
		}

	}

	void RegisterSaveDataMethodTypes(TypeRegistry& registry)
	{
		registry.Enum<SaveDataTarget>("SaveDataTarget", "Which save-data store savedata.get reads.")
			.Entry(SaveDataTarget::Editor, "Editor", "The play store: what a live play session starts with and its SaveData.Commit writes.")
			.Entry(SaveDataTarget::Play, "Play", "The running play session's store, as its scripts see it now.");
		registry.Struct<SaveDataGetParams>("SaveDataGetParams", "The params of savedata.get.")
			.Field("target", &SaveDataGetParams::Target, "The store to read: Editor (the default) or Play.");
		registry.Struct<SaveDataGetResult>("SaveDataGetResult", "A save-data store's contents.")
			.Field("values", &SaveDataGetResult::Values, "The values by key, keys sorted, numbers as SaveData.Get returns them.")
			.Field("count", &SaveDataGetResult::Count, "The number of keys.")
			.Field("bytes", &SaveDataGetResult::Bytes, std::format("The store's compact JSON size in bytes, at most {}.", MaxSaveDataBytes))
			.Field("loadError", &SaveDataGetResult::LoadError,
				"Editor: why the play store's file could not be read (it was moved aside as Store.corrupt-<n>.json and the store is empty); "
				"empty when it was read.");
		registry.Struct<SaveDataSetParams>("SaveDataSetParams", "The params of savedata.set.")
			.Field("values", &SaveDataSetParams::Values,
				std::format("An object of values by key (1 to {} bytes each): booleans, finite numbers, strings, arrays and objects nesting at most "
							"{} levels; null deletes a key.",
					MaxSaveDataKeyBytes, MaxSaveDataDepth))
			.Field("merge", &SaveDataSetParams::Merge, "Merge values into the play store instead of replacing it.");
		registry.Struct<SaveDataWriteResult>("SaveDataWriteResult", "The play store after a write.")
			.Field("count", &SaveDataWriteResult::Count, "The number of keys.")
			.Field("bytes", &SaveDataWriteResult::Bytes, "The store's compact JSON size in bytes.")
			.Field("loadError", &SaveDataWriteResult::LoadError,
				"savedata.set with merge: why the play store's file could not be read (it was moved aside as Store.corrupt-<n>.json and the "
				"values were merged into an empty store); empty when it was read, and for a write that replaces the store.");
	}

	void RegisterSaveDataMethods(MethodRegistry& methods)
	{
		methods.Add(
			{
				.Name = "savedata.get",
				.Description = "Reads editor play's save data: the play store (the project's Library/SaveData/Store.json, a read-only editor's "
							   "private store), which a live play session starts with and SaveData.Commit writes, or with target Play the "
							   "running session's store.",
				.ExposeAsTool = true,
				.Examples = { { .Description = "Read the play store.", .Params = Json::object() },
					{ .Description = "Read the running session's store.", .Params = { { "target", "Play" } } } },
			},
			&Automation::SaveDataGet);
		methods.Add(
			{
				.Name = "savedata.set",
				.Description = "Seeds editor play's save data: replaces the play store with values (or merges them with merge: true), so the "
							   "next live play session and the next input.record start with them. The running session keeps its own store.",
				.RequiredParams = { "values" },
				.ExposeAsTool = true,
				.Examples = { { .Description = "Unlock the second realm for the next play.", .Params = { { "values", { { "Realm", 2 }, { "Volume", 0.5 } } } } } },
			},
			&Automation::SaveDataSet);
		methods.Add(
			{
				.Name = "savedata.clear",
				.Description = "Empties editor play's save data (the play store), as a first run of the game finds it.",
				.ExposeAsTool = true,
				.Examples = { { .Description = "Start the next play without save data.", .Params = Json::object() } },
			},
			&Automation::SaveDataClear);
	}

}
