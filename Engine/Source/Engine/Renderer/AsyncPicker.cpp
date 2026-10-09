#include "EnginePCH.h"
#include "Engine/Renderer/AsyncPicker.h"

#include "Engine/Graphics/GraphicsDevice.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <format>
#include <limits>
#include <utility>
#include <vector>

namespace Engine {

	struct AsyncPicker::State
	{
		struct Slot
		{
			nvrhi::StagingTextureHandle Staging{};
			nvrhi::CommandListHandle Commands{};
			nvrhi::EventQueryHandle Query{};
			std::vector<UUID> Table{};
			PickRequest Request{};
			PickTicket Ticket{};
			uint64_t Submission = 0;
			bool Cancelled = false;
		};
		GraphicsDevice* Device = nullptr; // documented back-reference; device outlives picker
		std::array<Slot, MaxPendingPicks> Slots{};
		std::vector<uint64_t> Tombstones{};
		uint64_t NextTicket = 1;

		bool IsComplete(const Slot& slot, uint64_t completed) const
		{
			return slot.Submission <= completed && Device->GetNvrhiDevice()->pollEventQuery(slot.Query);
		}

		void Release(Slot& slot)
		{
			slot.Table.clear();
			slot.Ticket = {};
			slot.Submission = 0;
			slot.Cancelled = false;
		}

		void CollectCancelled()
		{
			const uint64_t completed = Device->GetCompletedSubmissionID();
			for (Slot& slot : Slots)
			{
				if (slot.Ticket.Value != 0 && slot.Cancelled && IsComplete(slot, completed))
					Release(slot);
			}
		}

		void RememberCancellation(uint64_t ticket)
		{
			Tombstones.insert(std::lower_bound(Tombstones.begin(), Tombstones.end(), ticket), ticket);
			if (Tombstones.size() > MaxPendingPicks)
				Tombstones.erase(Tombstones.begin());
		}
	};

	AsyncPicker::AsyncPicker(GraphicsDevice& device)
		: m_State(CreateScope<State>())
	{
		m_State->Device = &device;
	}

	AsyncPicker::~AsyncPicker() = default;

	Result<PickTicket> AsyncPicker::Request(nvrhi::ITexture& entityIds, std::span<const UUID> pickTable,
		uint64_t viewGeneration, const PickRequest& request)
	{
		State& state = *m_State;
		state.CollectCancelled();
		const nvrhi::TextureDesc& desc = entityIds.getDesc();
		if (desc.format != nvrhi::Format::R32_UINT || desc.dimension != nvrhi::TextureDimension::Texture2D
			|| desc.sampleCount != 1 || desc.arraySize != 1 || desc.depth != 1)
			return MakeError(ErrorCode::InvalidArgument, "Picking needs a single-sample two-dimensional R32_UINT image");
		if (request.X >= desc.width || request.Y >= desc.height)
		{
			ErrorLocation location{};
			location.JsonPointer = request.X >= desc.width ? "/x" : "/y";
			return std::unexpected(Error(ErrorCode::InvalidArgument, std::format("Picking pixel ({}, {}) is outside {} x {}", request.X, request.Y, desc.width, desc.height))
					.WithLocation(std::move(location)));
		}
		if (viewGeneration == 0 || request.ViewGeneration != viewGeneration)
			return MakeError(ErrorCode::InvalidArgument, "Picking request does not identify the current image generation");
		if (pickTable.size() > std::numeric_limits<uint32_t>::max())
			return MakeError(ErrorCode::InvalidArgument, "Picking table exceeds the R32_UINT index range");
		std::vector<UUID> sorted(pickTable.begin(), pickTable.end());
		std::ranges::sort(sorted);
		if ((!sorted.empty() && !sorted.front().IsValid()) || std::adjacent_find(sorted.begin(), sorted.end()) != sorted.end())
			return MakeError(ErrorCode::InvalidArgument, "Picking table contains duplicate or invalid UUIDs");
		const auto available = std::ranges::find_if(state.Slots, [](const State::Slot& slot)
		{
			return slot.Ticket.Value == 0;
		});
		if (available == state.Slots.end())
		{
			ErrorLocation location{};
			location.JsonPointer = "";
			return std::unexpected(Error(ErrorCode::Conflict, std::format("All {} asynchronous picking slots are outstanding", MaxPendingPicks))
					.WithLocation(std::move(location))
					.WithHint("Poll or cancel outstanding tickets before requesting another pixel"));
		}
		if (state.NextTicket == 0)
			return MakeError(ErrorCode::Conflict, "Asynchronous picking tickets are exhausted");
		State::Slot& slot = *available;
		if (!slot.Staging)
		{
			nvrhi::TextureDesc staging;
			staging.width = 1;
			staging.height = 1;
			staging.format = nvrhi::Format::R32_UINT;
			staging.debugName = "AsyncPicker.Pixel";
			ENGINE_TRY_ASSIGN(slot.Staging, state.Device->CreateStagingTexture(staging, nvrhi::CpuAccessMode::Read));
		}
		if (!slot.Commands)
		{
			ENGINE_TRY_ASSIGN(slot.Commands, state.Device->CreateCommandList(nvrhi::CommandListParameters().setEnableImmediateExecution(false)));
		}
		if (!slot.Query)
		{
			ENGINE_TRY_ASSIGN(slot.Query, state.Device->CreateEventQuery());
		}
		slot.Table.assign(pickTable.begin(), pickTable.end());
		slot.Request = request;
		slot.Commands->open();
		slot.Commands->copyTexture(slot.Staging, nvrhi::TextureSlice().setWidth(1).setHeight(1), &entityIds,
			nvrhi::TextureSlice().setOrigin(request.X, request.Y).setWidth(1).setHeight(1));
		slot.Commands->close();
		slot.Submission = state.Device->ExecuteCommandList(*slot.Commands);
		state.Device->GetNvrhiDevice()->resetEventQuery(slot.Query);
		state.Device->GetNvrhiDevice()->setEventQuery(slot.Query, nvrhi::CommandQueue::Graphics);
		slot.Ticket = { state.NextTicket++ };
		return slot.Ticket;
	}

	Result<std::optional<PickResult>> AsyncPicker::Poll(PickTicket ticket, uint64_t currentFrameIndex, uint64_t viewGeneration)
	{
		State& state = *m_State;
		state.CollectCancelled();
		const auto cancelled = std::ranges::find(state.Tombstones, ticket.Value);
		if (cancelled != state.Tombstones.end())
		{
			state.Tombstones.erase(cancelled);
			return MakeError(ErrorCode::Cancelled, "Picking ticket {} was cancelled", ticket.Value);
		}
		const auto found = std::ranges::find_if(state.Slots, [ticket](const State::Slot& slot)
		{
			return slot.Ticket == ticket && !slot.Cancelled;
		});
		if (ticket.Value == 0 || found == state.Slots.end())
			return MakeError(ErrorCode::NotFound, "Unknown or consumed picking ticket {}", ticket.Value);
		State::Slot& slot = *found;
		if (viewGeneration != slot.Request.ViewGeneration || currentFrameIndex < slot.Request.FrameIndex)
		{
			slot.Cancelled = true;
			state.CollectCancelled();
			return MakeError(ErrorCode::Cancelled, "Picking ticket {} has a stale frame or image generation", ticket.Value);
		}
		if (currentFrameIndex - slot.Request.FrameIndex < MinimumFrameDelay || !state.IsComplete(slot, state.Device->GetCompletedSubmissionID()))
			return std::optional<PickResult>{};
		size_t rowPitch = 0;
		nvrhi::IDevice* device = state.Device->GetNvrhiDevice();
		const void* mapped = device->mapStagingTexture(slot.Staging, nvrhi::TextureSlice(), nvrhi::CpuAccessMode::Read, &rowPitch);
		if (!mapped || rowPitch < sizeof(uint32_t))
		{
			if (mapped)
				device->unmapStagingTexture(slot.Staging);
			state.Release(slot);
			return MakeError(ErrorCode::Gpu, "Cannot map the completed picking pixel");
		}
		PickResult result;
		std::memcpy(&result.PickId, mapped, sizeof(result.PickId));
		device->unmapStagingTexture(slot.Staging);
		if (result.PickId != 0 && result.PickId <= slot.Table.size())
			result.Entity = slot.Table[result.PickId - 1];
		result.FrameIndex = slot.Request.FrameIndex;
		result.SceneRevision = slot.Request.SceneRevision;
		result.Sequence = slot.Request.Sequence;
		result.ViewGeneration = slot.Request.ViewGeneration;
		state.Release(slot);
		return std::optional<PickResult>{ result };
	}

	void AsyncPicker::CancelAll()
	{
		State& state = *m_State;
		for (State::Slot& slot : state.Slots)
		{
			if (slot.Ticket.Value != 0 && !slot.Cancelled)
			{
				state.RememberCancellation(slot.Ticket.Value);
				slot.Cancelled = true;
			}
		}
		state.CollectCancelled();
	}

}
