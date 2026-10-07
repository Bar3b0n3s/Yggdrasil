#include "EnginePCH.h"
#include "Engine/Automation/Protocol/Dispatcher.h"

#include "Engine/Automation/Protocol/MethodRegistry.h"
#include "Engine/Automation/Protocol/Watchdog.h"
#include "Engine/Core/Assert.h"
#include "Engine/Core/FatalError.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/RingBufferSink.h"
#include "Engine/Reflection/FuzzySuggest.h"

#include <algorithm>
#include <deque>
#include <exception>
#include <format>
#include <map>
#include <new>
#include <system_error>

// This file is the protocol's allowlisted try/catch boundary (Architecture §4.6 item 5). Only the three calls into method
// code are guarded (the handler through MethodRegistry::Invoke, PendingOperation::Poll and PendingOperation::Cancel):
// std::bad_alloc is fatal like everywhere else (§4.6 "Fatal environment"); a std::system_error goes to the host's
// SystemErrorHandler first (a Vulkan error ends the process there); any other std::exception, and a system error the
// handler returns from, asserts in Debug builds and becomes an Internal error response in the others, so a Release editor
// keeps serving its other clients.
//
// Handlers may change the client set while they run (a session.shutdown that disconnects, an in-process client that
// removes itself), so the pump never holds a reference into the client map across a call into the host or a method: it
// looks the client up again afterwards, and an operation whose client vanished meanwhile is cancelled.

namespace Engine {

	struct Dispatcher::State
	{
		// A request that returned a PendingOperation, with the context it is polled with.
		struct PendingRequest
		{
			Scope<MethodContext> Context; // destroyed after the operation (reverse member order)
			Scope<PendingOperation> Operation;
			Json Id{};
			bool IsNotification = false;
			uint64_t StartOrder = 0; // pending operations are polled in the order they started
		};

		struct Client
		{
			std::string Name{};
			bool Offload = true;
			std::deque<RpcRequest> Queue{};
			Scope<PendingRequest> Pending; // at most one: the client's next request waits until it resolves
		};

		const MethodRegistry* Registry = nullptr; // documented back-references
		IMethodHost* Host = nullptr;
		Watchdog* PhaseMarker = nullptr;
		DispatcherSpecification Specification{};
		MetaBuilder Meta;
		uint64_t NextOffloadSequence = 1;
		uint64_t NextStartOrder = 1;
		std::map<ClientId, Client> Clients{}; // id order is the round-robin order
		ClientId LastServed = NoClient;

		explicit State(const RingBufferSink& log)
			: Meta(log)
		{
		}

		// The steps of Pump (see the Dispatcher class comment), appending responses to `messages`.
		void PollPending(ClientId client, std::vector<OutboundMessage>& messages);
		void RunRequest(ClientId client, RpcRequest request, std::vector<OutboundMessage>& messages);
		// Cancels a pending operation without answering it (§13.2 "Disconnect"): Cancel between EnterInvocation and
		// LeaveInvocation, then FinishRequest.
		void Cancel(PendingRequest& pending);
		void RespondResult(ClientId client, const Json& id, bool isNotification, Json result, const MethodContext& context,
			std::vector<OutboundMessage>& messages);
		// `meta`: the "_meta" already built for this response, or null to build it now.
		void RespondError(ClientId client, const Json& id, bool isNotification, RpcErrorCode code, const Error& error, const Json& extraData,
			std::vector<OutboundMessage>& messages, const Json* meta = nullptr);
	};

	namespace Utils {

		// The error an exception escaping method code becomes (after asserting in Debug builds).
		[[nodiscard]] static Error MakeExceptionError(std::string_view method, std::string_view what)
		{
#if defined(ENGINE_DEBUG)
			ENGINE_CORE_ASSERT(false, "Automation method '{}' let an exception escape: {}", method, what);
#endif
			return Error(ErrorCode::Unknown, std::format("method '{}' failed with an unexpected exception: {}", method, what));
		}

		// A std::system_error escaping method code: the host's handler first (it may end the process), then the error of
		// any other exception.
		[[nodiscard]] static Error MakeSystemErrorError(const SystemErrorHandler& handler, std::string_view method, const std::system_error& error)
		{
			if (handler)
				handler(error, method);
			return MakeExceptionError(method, error.what());
		}

		[[nodiscard]] static MethodResult InvokeGuarded(const MethodRegistry& registry, MethodContext& context, const SystemErrorHandler& systemErrors)
		{
			try
			{
				return registry.Invoke(context);
			}
			catch (const std::bad_alloc&)
			{
				FatalError(FatalErrorKind::OutOfMemory, "std::bad_alloc: out of memory in an automation method");
			}
			catch (const std::system_error& error)
			{
				return MakeSystemErrorError(systemErrors, context.GetRequest().Method, error);
			}
			catch (const std::exception& exception)
			{
				return MakeExceptionError(context.GetRequest().Method, exception.what());
			}
		}

		[[nodiscard]] static std::optional<Result<Json>> PollGuarded(PendingOperation& operation, MethodContext& context,
			const SystemErrorHandler& systemErrors)
		{
			try
			{
				return operation.Poll(context);
			}
			catch (const std::bad_alloc&)
			{
				FatalError(FatalErrorKind::OutOfMemory, "std::bad_alloc: out of memory in an automation operation");
			}
			catch (const std::system_error& error)
			{
				return Result<Json>(std::unexpected(MakeSystemErrorError(systemErrors, context.GetRequest().Method, error)));
			}
			catch (const std::exception& exception)
			{
				return Result<Json>(std::unexpected(MakeExceptionError(context.GetRequest().Method, exception.what())));
			}
		}

		static void CancelGuarded(PendingOperation& operation, MethodContext& context, const SystemErrorHandler& systemErrors)
		{
			try
			{
				operation.Cancel(context);
			}
			catch (const std::bad_alloc&)
			{
				FatalError(FatalErrorKind::OutOfMemory, "std::bad_alloc: out of memory while cancelling an automation operation");
			}
			catch (const std::system_error& error)
			{
				ENGINE_CORE_ERROR("{}", MakeSystemErrorError(systemErrors, context.GetRequest().Method, error).GetMessageText());
			}
			catch (const std::exception& exception)
			{
				ENGINE_CORE_ERROR("{}", MakeExceptionError(context.GetRequest().Method, exception.what()).GetMessageText());
			}
		}

		// The phase marker text while `operation` of `method` runs.
		[[nodiscard]] static std::string GetOperationPhase(const PendingOperation& operation, std::string_view method)
		{
			std::string phase = operation.GetPhase();
			return phase.empty() ? std::format("Automation:{}", method) : phase;
		}

	}

	void Dispatcher::State::PollPending(ClientId client, std::vector<OutboundMessage>& messages)
	{
		const auto found = Clients.find(client);
		if (found == Clients.end() || found->second.Pending == nullptr)
			return;
		Scope<PendingRequest> pending = std::move(found->second.Pending);

		MethodContext& context = *pending->Context;
		Host->EnterInvocation(context);
		std::optional<Result<Json>> outcome;
		{
			const WatchdogPhaseScope phase(PhaseMarker, Utils::GetOperationPhase(*pending->Operation, context.GetRequest().Method));
			outcome = Utils::PollGuarded(*pending->Operation, context, Specification.SystemErrors);
		}
		Host->LeaveInvocation(context);

		const auto owner = Clients.find(client);
		if (!outcome.has_value())
		{
			if (owner != Clients.end())
				owner->second.Pending = std::move(pending);
			else
				Cancel(*pending); // the client went away while the operation ran
			return;
		}
		Host->FinishRequest(context);
		if (*outcome)
		{
			RespondResult(client, pending->Id, pending->IsNotification, std::move(**outcome), context, messages);
			return;
		}
		const Error& error = outcome->error();
		RespondError(client, pending->Id, pending->IsNotification, ToRpcErrorCode(error.GetCode()), error, context.GetErrorData(), messages);
	}

	void Dispatcher::State::RunRequest(ClientId client, RpcRequest request, std::vector<OutboundMessage>& messages)
	{
		const MethodDescriptor* method = Registry->Find(request.Method);
		if (method == nullptr)
		{
			std::vector<std::string> suggestions = Registry->SuggestMethodNames(request.Method);
			std::string hint = MakeDidYouMeanHint(suggestions);
			const std::string message = std::format("no method '{}'", request.Method);
			const Error error = Error(ErrorCode::NotFound, message)
									.WithHint(hint)
									.WithIssue(ErrorIssue{ .JsonPointer = "/method", .Message = message, .Hint = hint, .Suggestions = std::move(suggestions) });
			RespondError(client, request.Id, request.IsNotification, RpcErrorCode::MethodNotFound, error, Json(), messages);
			return;
		}

		const Status available = Host->CheckAvailability(*method);
		if (!available)
		{
			RespondError(client, request.Id, request.IsNotification, ToRpcErrorCode(available.error().GetCode()), available.error(), Json(),
				messages);
			return;
		}

		Result<PreparedParams> prepared = Registry->PrepareParams(*method, request.Params);
		if (!prepared)
		{
			RespondError(client, request.Id, request.IsNotification, ToRpcErrorCode(prepared.error().GetCode()), prepared.error(), Json(),
				messages);
			return;
		}

		const auto found = Clients.find(client);
		RequestInfo info{
			.Client = client,
			.ClientName = found != Clients.end() ? found->second.Name : std::string(),
			.Id = request.Id,
			.Method = request.Method,
			.TranscriptLine = request.TranscriptLine,
		};
		Scope<MethodContext> context = Host->CreateContext(MethodRequest{
			.Info = std::move(info),
			.Options = prepared->Options,
			.Method = method,
			.Params = std::move(prepared->Params),
			.Registry = Registry,
			.PhaseMarker = PhaseMarker,
			.NestingDepth = 0,
		});
		ENGINE_CORE_ASSERT(context != nullptr, "IMethodHost::CreateContext returned no context for '{}'", request.Method);
		if (context == nullptr)
		{
			RespondError(client, request.Id, request.IsNotification, RpcErrorCode::Internal,
				Error(ErrorCode::Unknown, std::format("the host created no context for '{}'", request.Method)), Json(), messages);
			return;
		}

		const Status admitted = Host->AdmitRequest(*context);
		if (!admitted)
		{
			Host->FinishRequest(*context);
			RespondError(client, request.Id, request.IsNotification, ToRpcErrorCode(admitted.error().GetCode()), admitted.error(),
				context->GetErrorData(), messages);
			return;
		}

		Host->EnterInvocation(*context);
		MethodResult result;
		{
			const WatchdogPhaseScope phase(PhaseMarker, std::format("Automation:{}", request.Method));
			result = Utils::InvokeGuarded(*Registry, *context, Specification.SystemErrors);
		}
		Host->LeaveInvocation(*context);

		if (Scope<PendingOperation>* operation = std::get_if<Scope<PendingOperation>>(&result))
		{
			Scope<PendingRequest> pending = CreateScope<PendingRequest>();
			pending->Context = std::move(context);
			pending->Operation = std::move(*operation);
			pending->Id = std::move(request.Id);
			pending->IsNotification = request.IsNotification;
			pending->StartOrder = NextStartOrder++;
			const auto owner = Clients.find(client);
			if (owner == Clients.end())
			{
				Cancel(*pending);
				return;
			}
			owner->second.Pending = std::move(pending);
			// Its first frame of work is this one.
			PollPending(client, messages);
			return;
		}

		Host->FinishRequest(*context);
		if (Json* json = std::get_if<Json>(&result))
		{
			RespondResult(client, request.Id, request.IsNotification, std::move(*json), *context, messages);
			return;
		}
		const Error& error = std::get<Error>(result);
		RespondError(client, request.Id, request.IsNotification, ToRpcErrorCode(error.GetCode()), error, context->GetErrorData(), messages);
	}

	void Dispatcher::State::Cancel(PendingRequest& pending)
	{
		MethodContext& context = *pending.Context;
		Host->EnterInvocation(context);
		{
			const WatchdogPhaseScope phase(PhaseMarker, Utils::GetOperationPhase(*pending.Operation, context.GetRequest().Method));
			Utils::CancelGuarded(*pending.Operation, context, Specification.SystemErrors);
		}
		Host->LeaveInvocation(context);
		Host->FinishRequest(context);
	}

	void Dispatcher::State::RespondResult(ClientId client, const Json& id, bool isNotification, Json result, const MethodContext& context,
		std::vector<OutboundMessage>& messages)
	{
		const auto found = Clients.find(client);
		if (isNotification || found == Clients.end())
			return;
		const Json meta = Meta.Build(client, Host->GetMetaState());
		if (!result.is_object())
		{
			RespondError(client, id, isNotification, RpcErrorCode::Internal,
				Error(ErrorCode::Unknown, std::format("method '{}' produced a result that is not an object", context.GetRequest().Method)), Json(),
				messages, &meta);
			return;
		}

		if (found->second.Offload)
		{
			// Measured on the minified result without "_meta" (ADR 0008 decision 22); the file holds it indented, for reading.
			const std::string text = result.dump(-1, ' ', false, Json::error_handler_t::replace);
			if (text.size() > Specification.OffloadThresholdBytes)
			{
				const std::string fileName = MakeOffloadFileName(Host->GetOffloadServerTag(), NextOffloadSequence++);
				const std::string file = result.dump(1, '\t', false, Json::error_handler_t::replace);
				Result<std::string> path = Host->WriteOffloadedResult(fileName, file);
				if (!path)
				{
					const Error error = std::move(path).error().WithContext(
						std::format("while offloading the {}-byte result of '{}'", text.size(), context.GetRequest().Method));
					RespondError(client, id, isNotification, ToRpcErrorCode(error.GetCode()), error, Json(), messages, &meta);
					return;
				}
				result = MakeOffloadedResult(*path, MakeOffloadSummary(result));
			}
		}
		if (context.IsDryRun())
			result["dryRun"] = true;
		messages.push_back(OutboundMessage{ .Client = client, .Message = MakeResultResponse(id, std::move(result), meta) });
	}

	void Dispatcher::State::RespondError(ClientId client, const Json& id, bool isNotification, RpcErrorCode code, const Error& error,
		const Json& extraData, std::vector<OutboundMessage>& messages, const Json* meta)
	{
		if (isNotification || !Clients.contains(client))
			return;
		const Json built = meta != nullptr ? Json() : Meta.Build(client, Host->GetMetaState());
		messages.push_back(OutboundMessage{ .Client = client, .Message = MakeErrorResponse(id, code, error, extraData, meta != nullptr ? *meta : built) });
	}

	Dispatcher::Dispatcher(const MethodRegistry& registry, IMethodHost& host, const RingBufferSink& log, Watchdog* watchdog,
		DispatcherSpecification specification)
		: m_State(CreateScope<State>(log))
	{
		ENGINE_CORE_ASSERT(registry.IsFrozen(), "The dispatcher needs a frozen method registry");
		m_State->Registry = &registry;
		m_State->Host = &host;
		m_State->PhaseMarker = watchdog;
		m_State->Specification = specification;
	}

	Dispatcher::~Dispatcher()
	{
		std::vector<ClientId> clients;
		for (const auto& [client, state] : m_State->Clients)
			clients.push_back(client);
		for (const ClientId client : clients)
			RemoveClient(client);
	}

	void Dispatcher::AddClient(ClientId client, std::string name, bool offloadLargeResults)
	{
		ENGINE_CORE_ASSERT(client != NoClient && !m_State->Clients.contains(client), "Dispatcher::AddClient: client {} is invalid or known", client);
		if (client == NoClient || m_State->Clients.contains(client))
			return;
		State::Client& state = m_State->Clients[client];
		state.Name = std::move(name);
		state.Offload = offloadLargeResults;
		m_State->Meta.AddClient(client);
	}

	void Dispatcher::RemoveClient(ClientId client)
	{
		const auto found = m_State->Clients.find(client);
		if (found == m_State->Clients.end())
			return;
		Scope<State::PendingRequest> pending = std::move(found->second.Pending);
		const std::string name = std::move(found->second.Name);
		const size_t dropped = found->second.Queue.size();
		m_State->Clients.erase(found);
		m_State->Meta.RemoveClient(client);

		size_t cancelled = 0;
		if (pending != nullptr)
		{
			m_State->Cancel(*pending);
			pending.reset();
			cancelled = 1;
		}
		ENGINE_CORE_INFO("Automation client '{}' ({}) removed: {} pending operation(s) cancelled, {} queued request(s) dropped", name, client,
			cancelled, dropped);
	}

	void Dispatcher::Enqueue(ClientId client, RpcRequest request)
	{
		const auto found = m_State->Clients.find(client);
		ENGINE_CORE_ASSERT(found != m_State->Clients.end(), "Dispatcher::Enqueue: unknown client {}", client);
		if (found != m_State->Clients.end())
			found->second.Queue.push_back(std::move(request));
	}

	std::vector<OutboundMessage> Dispatcher::Pump(std::chrono::microseconds budget)
	{
		const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
		std::vector<OutboundMessage> messages;
		const auto heartbeat = [this]()
		{
			if (m_State->PhaseMarker != nullptr)
				m_State->PhaseMarker->Heartbeat(std::chrono::steady_clock::now());
		};

		// Every pending operation once, in the order they started.
		std::vector<std::pair<uint64_t, ClientId>> pending;
		for (const auto& [client, state] : m_State->Clients)
		{
			if (state.Pending != nullptr)
				pending.emplace_back(state.Pending->StartOrder, client);
		}
		std::sort(pending.begin(), pending.end());
		for (const auto& [order, client] : pending)
		{
			m_State->PollPending(client, messages);
			heartbeat();
		}

		// Then queued requests, clients in turn, until none is runnable or the budget is spent (at least one runs).
		const auto isRunnable = [](const State::Client& state)
		{
			return !state.Queue.empty() && state.Pending == nullptr;
		};
		bool ranOne = false;
		while (!ranOne || std::chrono::steady_clock::now() - start < budget)
		{
			ClientId next = NoClient;
			for (auto it = m_State->Clients.upper_bound(m_State->LastServed); it != m_State->Clients.end() && next == NoClient; ++it)
			{
				if (isRunnable(it->second))
					next = it->first;
			}
			for (auto it = m_State->Clients.begin(); it != m_State->Clients.end() && next == NoClient && it->first <= m_State->LastServed; ++it)
			{
				if (isRunnable(it->second))
					next = it->first;
			}
			if (next == NoClient)
				break;

			m_State->LastServed = next;
			State::Client& state = m_State->Clients[next];
			RpcRequest request = std::move(state.Queue.front());
			state.Queue.pop_front();
			m_State->RunRequest(next, std::move(request), messages);
			ranOne = true;
			heartbeat();
		}
		return messages;
	}

	bool Dispatcher::HasWork() const
	{
		for (const auto& [client, state] : m_State->Clients)
		{
			if (!state.Queue.empty() || state.Pending != nullptr)
				return true;
		}
		return false;
	}

	size_t Dispatcher::GetQueuedCount(ClientId client) const
	{
		const auto found = m_State->Clients.find(client);
		return found != m_State->Clients.end() ? found->second.Queue.size() : 0;
	}

	size_t Dispatcher::GetPendingCount(ClientId client) const
	{
		const auto found = m_State->Clients.find(client);
		return found != m_State->Clients.end() && found->second.Pending != nullptr ? 1 : 0;
	}

}
