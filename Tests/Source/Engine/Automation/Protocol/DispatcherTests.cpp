#include "TestsPCH.h"

#include "Engine/Automation/Protocol/Dispatcher.h"

#include "Engine/Automation/Protocol/MethodRegistry.h"
#include "Engine/Automation/Protocol/Watchdog.h"
#include "Engine/Core/FatalError.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/RingBufferSink.h"
#include "Support/DeathTest.h"
#include "Support/ProtocolTestTypes.h"

namespace Engine {

	namespace {

		// Fault injection at the protocol host boundary; normal operations delegate to the existing test host.
		class DispatcherOutcomeHost final : public IMethodHost
		{
		public:
			explicit DispatcherOutcomeHost(Test::TestHostState& state)
				: m_Host(state)
			{
			}
			[[nodiscard]] Status CheckAvailability(const MethodDescriptor& method) const override { return m_Host.CheckAvailability(method); }
			[[nodiscard]] Scope<MethodContext> CreateContext(MethodRequest request) override { return m_Host.CreateContext(std::move(request)); }
			[[nodiscard]] Status AdmitRequest(MethodContext& context) override { return m_Host.AdmitRequest(context); }
			void EnterInvocation(MethodContext& context) override { m_Host.EnterInvocation(context); }
			void LeaveInvocation(MethodContext& context) override
			{
				m_Host.LeaveInvocation(context);
				if (AfterInvocation)
					AfterInvocation();
			}
			void FinishRequest(MethodContext& context) override { m_Host.FinishRequest(context); }
			[[nodiscard]] MetaState GetMetaState() const override { return m_Host.GetMetaState(); }
			[[nodiscard]] Result<std::string> WriteOffloadedResult(std::string_view text) override
			{
				++OffloadAttempts;
				if (BeforeOffload)
					BeforeOffload();
				if (FailOffload)
					return MakeError(ErrorCode::Io, "injected offload failure");
				return m_Host.WriteOffloadedResult(text);
			}
			std::function<void()> AfterInvocation{};
			std::function<void()> BeforeOffload{};
			bool FailOffload = false;
			uint32_t OffloadAttempts = 0;
		private:
			Test::TestMethodHost m_Host;
		};

		// A real pending operation returning deliberate errors or malformed output to exercise dispatcher outcomes.
		class DispatcherOutcomeOperation final : public PendingOperation
		{
		public:
			explicit DispatcherOutcomeOperation(uint32_t mode)
				: m_Mode(mode)
			{
			}
			[[nodiscard]] std::optional<Result<Json>> Poll(MethodContext&) override
			{
				if (m_Mode == 0)
					return Result<Json>(MakeError(ErrorCode::NotFound, "injected pending failure"));
				return Json::array();
			}
			void Cancel(MethodContext&) override {}
		private:
			uint32_t m_Mode = 0;
		};

		[[nodiscard]] static Result<Scope<PendingOperation>> BeginDispatcherOutcome(Test::TestHostContext&, const Test::PendParams& params)
		{
			return Scope<PendingOperation>(CreateScope<DispatcherOutcomeOperation>(params.Polls));
		}

		// A pending operation that reports its progress (§13.2 "Progress"): it resolves at its poll `polls`, its work counts its
		// polls, and its stage turns from Suite to Replay at its fourth poll.
		class DispatcherProgressOperation final : public PendingOperation
		{
		public:
			explicit DispatcherProgressOperation(uint32_t polls)
				: m_Polls(polls)
			{
			}
			[[nodiscard]] std::optional<Result<Json>> Poll(MethodContext& context) override
			{
				++m_Done;
				if (m_Done < m_Polls)
					return std::nullopt;
				return context.SerializeResult(Test::PendResult{ .Polls = m_Done });
			}
			[[nodiscard]] std::optional<PendingProgress> GetProgress() const override
			{
				return PendingProgress{ .Work = m_Done, .Stage = m_Done < 4 ? PendingProgressStage::Suite : PendingProgressStage::Replay, .Subject = "Board/clears a line", .Tick = 10ULL * m_Done, .FinalTick = 0, .Step = 1, .Steps = 2 };
			}
		private:
			uint32_t m_Polls = 0;
			uint32_t m_Done = 0;
		};

		[[nodiscard]] static Result<Scope<PendingOperation>> BeginDispatcherProgress(Test::TestHostContext&, const Test::PendParams& params)
		{
			return Scope<PendingOperation>(CreateScope<DispatcherProgressOperation>(params.Polls));
		}

		// Notes the request the host's watchdog names ("request <method>", "request" for none) in the host's calls.
		static void NoteWatchdogRequest(MethodContext& context)
		{
			Test::TestHostState& state = static_cast<Test::TestHostContext&>(context).GetState();
			REQUIRE(state.PhaseMarker != nullptr);
			state.Calls.push_back(std::format("request {}", state.PhaseMarker->GetRequest()));
		}

		// A pending operation that never resolves and notes the watchdog's request on every Poll and on Cancel.
		class RequestObservingOperation final : public PendingOperation
		{
		public:
			[[nodiscard]] std::optional<Result<Json>> Poll(MethodContext& context) override
			{
				NoteWatchdogRequest(context);
				return std::nullopt;
			}
			void Cancel(MethodContext& context) override { NoteWatchdogRequest(context); }
		};

		[[nodiscard]] static Result<Scope<PendingOperation>> BeginRequestObserving(Test::TestHostContext& context, const Test::PendParams&)
		{
			NoteWatchdogRequest(context);
			return Scope<PendingOperation>(CreateScope<RequestObservingOperation>());
		}

		// The test's own clock for DispatcherSpecification::TimeSource: it moves only when the test advances it.
		struct ManualClock
		{
			std::chrono::steady_clock::time_point Now{};

			[[nodiscard]] std::function<std::chrono::steady_clock::time_point()> GetFunction()
			{
				return [this]()
				{
					return Now;
				};
			}
		};

		// The test types, methods and host, a ring of the test's own and a dispatcher over them, with clients 1 and 2.
		struct DispatcherSetup
		{
			Scope<TypeRegistry> Types = Test::CreateProtocolTestTypes();
			Scope<MethodRegistry> Methods;
			Test::TestHostState State;
			DispatcherOutcomeHost Host{ State };
			RingBufferSink Log{ 256 };
			Scope<Dispatcher> Calls;

			// `watchdog` (may be null): the dispatcher's, which the test host's operations read too (TestHostState::PhaseMarker).
			explicit DispatcherSetup(DispatcherSpecification specification = {}, Watchdog* watchdog = nullptr)
			{
				State.PhaseMarker = watchdog;
				Methods = CreateScope<MethodRegistry>(*Types);
				Test::RegisterProtocolTestMethods(*Methods);
				Methods->AddPending<Test::TestHostContext, Test::PendParams, Test::PendResult>(
					{ .Name = "test.observedOutcome", .Description = "Exercises pending result failure reporting.", .Examples = { { .Description = "Fail during Poll.", .Params = Json{ { "polls", 0 } } } } }, &BeginDispatcherOutcome);
				Methods->AddPending<Test::TestHostContext, Test::PendParams, Test::PendResult>(
					{ .Name = "test.progress", .Description = "A pending operation that reports its progress.", .Examples = { { .Description = "Resolve at the fifth poll.", .Params = Json{ { "polls", 5 } } } } }, &BeginDispatcherProgress);
				Methods->AddPending<Test::TestHostContext, Test::PendParams, Test::PendResult>(
					{ .Name = "test.observeRequest", .Description = "A pending operation that notes the watchdog's request.", .Examples = { { .Description = "Never resolves.", .Params = Json::object() } } }, &BeginRequestObserving);
				Methods->Freeze();
				Calls = CreateScope<Dispatcher>(*Methods, Host, Log, watchdog, specification);
				Calls->AddClient(1, "first");
				Calls->AddClient(2, "second");
			}

			// The host calls recorded for `method`, in order, without the method name.
			std::vector<std::string> CallsOf(std::string_view method) const
			{
				std::vector<std::string> calls;
				for (const std::string& call : State.Calls)
				{
					const size_t space = call.find(' ');
					if (space != std::string::npos && call.substr(space + 1) == method)
						calls.push_back(call.substr(0, space));
				}
				return calls;
			}

			// Pumps until no work is left (at most 100 pumps) and returns every message.
			std::vector<OutboundMessage> PumpAll()
			{
				std::vector<OutboundMessage> messages;
				for (int pump = 0; pump < 100 && Calls->HasWork(); ++pump)
				{
					for (OutboundMessage& message : Calls->Pump(std::chrono::microseconds(1000000)))
						messages.push_back(std::move(message));
				}
				return messages;
			}
		};

	}

	ENGINE_DEATH_TEST("Automation/DispatcherHandlerExceptionAssertsInDebug")
	{
		DispatcherSetup setup;
		setup.Calls->Enqueue(1, Test::MakeTestRequest(1, "test.throw", Json::object()));
		static_cast<void>(setup.Calls->Pump(std::chrono::microseconds(1000000)));
	}

	// A host whose SystemErrorHandler ends the process, as the editor's does for a Vulkan error (RaiseVulkanError).
	ENGINE_DEATH_TEST("Automation/DispatcherSystemErrorReachesTheHostHandler")
	{
		DispatcherSetup setup(DispatcherSpecification{ .SystemErrors = [](const std::system_error& /*error*/, std::string_view method) -> std::optional<Error>
		{
			FatalError(FatalErrorKind::Gpu, std::format("the host saw a std::system_error escape '{}'", method));
		} });
		setup.Calls->Enqueue(1, Test::MakeTestRequest(1, "test.systemError", Json::object()));
		static_cast<void>(setup.Calls->Pump(std::chrono::microseconds(1000000)));
	}

	// A host whose SystemErrorHandler returns: the error is then handled like any other exception, which asserts in Debug.
	ENGINE_DEATH_TEST("Automation/DispatcherReturningSystemErrorHandlerAsserts")
	{
		DispatcherSetup setup(DispatcherSpecification{ .SystemErrors = [](const std::system_error& /*error*/, std::string_view /*method*/) -> std::optional<Error>
		{
			return std::nullopt;
		} });
		setup.Calls->Enqueue(1, Test::MakeTestRequest(1, "test.systemError", Json::object()));
		static_cast<void>(setup.Calls->Pump(std::chrono::microseconds(1000000)));
	}

	TEST_SUITE("Automation")
	{
		TEST_CASE("Dispatcher: observers receive one terminal event for every request and notification")
		{
			for (bool notification : { false, true })
			{
				std::vector<DispatcherRequestEvent> events;
				DispatcherSetup setup({ .RequestObserver = [&events](const DispatcherRequestEvent& event)
				{
					events.push_back(event);
				} });
				const std::vector<std::pair<std::string, Json>> requests{
					{ "test.read", Json::object() }, { "unknown.method", Json::object() },
					{ "test.echo", Json::object() }, { "test.fail", Json::object() }
				};
				for (const auto& [method, params] : requests)
				{
					RpcRequest request = Test::MakeTestRequest(1, method, params);
					request.Id = notification ? Json() : Json("same-wire-id");
					request.IsNotification = notification;
					request.TranscriptLine = 45;
					setup.Calls->Enqueue(1, std::move(request));
				}
				const auto messages = setup.PumpAll();
				CHECK(messages.size() == (notification ? 0 : requests.size()));
				REQUIRE(events.size() == requests.size() * 2);
				for (size_t index = 0; index < requests.size(); ++index)
				{
					const auto& start = events[index * 2];
					const auto& end = events[index * 2 + 1];
					CHECK(start.Sequence == index + 1);
					CHECK(start.Sequence == end.Sequence);
					CHECK(start.Phase == DispatcherRequestPhase::Started);
					CHECK(end.Phase == (index == 0 ? DispatcherRequestPhase::Succeeded : DispatcherRequestPhase::Failed));
					CHECK(start.Request.Client == 1);
					CHECK(start.Request.ClientName == "first");
					CHECK(start.Request.Method == requests[index].first);
					CHECK(end.Request.Method == start.Request.Method);
					CHECK(start.Request.Id == (notification ? Json() : Json("same-wire-id")));
					CHECK(end.Request.Id == start.Request.Id);
					CHECK(end.Request.TranscriptLine == 45);
				}
				setup.State.LauncherState = true;
				setup.Calls->Enqueue(1, Test::MakeTestRequest(5, "test.echo", Json{ { "text", "unavailable" } }));
				REQUIRE(setup.PumpAll().size() == 1);
				CHECK(events.back().Phase == DispatcherRequestPhase::Failed);
				setup.State.DenyEverything = true;
				setup.Calls->Enqueue(1, Test::MakeTestRequest(6, "test.read", Json::object()));
				REQUIRE(setup.PumpAll().size() == 1);
				CHECK(events.back().Phase == DispatcherRequestPhase::Failed);
				CHECK(events.size() == 12);
			}
		}

		TEST_CASE("Dispatcher: protocol admission precedes lookup and never readmits pending work")
		{
			std::vector<DispatcherRequestEvent> events;
			bool reject = false;
			uint32_t admissions = 0;
			DispatcherSetup setup({ .RequestObserver = [&events](const DispatcherRequestEvent& event)
			{
				events.push_back(event);
			},
				.RequestAdmission = [&reject, &admissions, &events](const RequestInfo& request) -> std::optional<DispatcherRequestRejection>
			{
				++admissions;
				REQUIRE_FALSE(events.empty());
				CHECK(events.back().Phase == DispatcherRequestPhase::Started);
				CHECK(events.back().Request.Method == request.Method);
				if (reject)
					return DispatcherRequestRejection{ .Code = RpcErrorCode::Busy, .Failure = Error(ErrorCode::InvalidState, "paused") };
				return std::nullopt;
			} });
			setup.Calls->Enqueue(1, Test::MakeTestRequest(1, "test.pend", Json{ { "polls", 2 } }));
			setup.Calls->Enqueue(1, Test::MakeTestRequest(2, "unknown.method", Json::object()));
			CHECK(setup.Calls->Pump(std::chrono::seconds(1)).empty());
			CHECK(admissions == 1);
			CHECK(events.size() == 1);
			reject = true;
			const auto messages = setup.PumpAll();
			REQUIRE(messages.size() == 2);
			CHECK(messages[0].Message.contains("result"));
			CHECK(messages[1].Message["error"]["code"] == Json(std::to_underlying(RpcErrorCode::Busy)));
			CHECK(messages[1].Message["error"]["data"].contains("_meta"));
			CHECK(admissions == 2);
			REQUIRE(events.size() == 4);
			CHECK(events[1].Phase == DispatcherRequestPhase::Succeeded);
			CHECK(events[1].Sequence == 1);
			CHECK(events[2].Sequence == 2);
			CHECK(events[3].Phase == DispatcherRequestPhase::Failed);
			CHECK(setup.CallsOf("unknown.method").empty());
			RpcRequest notification = Test::MakeTestRequest(3, "test.echo", Json::object());
			notification.Id = Json();
			notification.IsNotification = true;
			setup.Calls->Enqueue(1, std::move(notification));
			CHECK(setup.PumpAll().empty());
			CHECK(admissions == 3);
			REQUIRE(events.size() == 6);
			CHECK(events[4].Sequence == 3);
			CHECK(events[5].Sequence == 3);
			CHECK(events[5].Phase == DispatcherRequestPhase::Failed);
			CHECK(setup.CallsOf("test.echo").empty());
		}

		TEST_CASE("Dispatcher: disconnect and destruction cancel starts and never observe dropped queues")
		{
			std::vector<DispatcherRequestEvent> events;
			DispatcherSetup setup({ .RequestObserver = [&events](const DispatcherRequestEvent& event)
			{
				events.push_back(event);
			} });
			setup.Calls->AddClient(3, "never-started");
			setup.Calls->Enqueue(3, Test::MakeTestRequest(1, "test.read", Json::object()));
			setup.Calls->RemoveClient(3);
			CHECK(events.empty());
			for (ClientId client : { ClientId{ 1 }, ClientId{ 2 } })
			{
				RpcRequest notification = Test::MakeTestRequest(1, "test.pend", Json{ { "polls", 1000 } });
				notification.Id = Json();
				notification.IsNotification = true;
				setup.Calls->Enqueue(client, std::move(notification));
				setup.Calls->Enqueue(client, Test::MakeTestRequest(2, "test.read", Json::object()));
			}
			CHECK(setup.Calls->Pump(std::chrono::seconds(1)).empty());
			REQUIRE(events.size() == 2);
			CHECK(events[0].Sequence == 1);
			CHECK(events[1].Sequence == 2);
			setup.Calls->RemoveClient(1);
			setup.Calls->RemoveClient(1);
			setup.Calls.reset();
			REQUIRE(events.size() == 4);
			CHECK(events[2].Sequence == 1);
			CHECK(events[3].Sequence == 2);
			CHECK(events[2].Phase == DispatcherRequestPhase::Cancelled);
			CHECK(events[3].Phase == DispatcherRequestPhase::Cancelled);
			CHECK(events[2].Request.ClientName == "first");
			CHECK(events[3].Request.ClientName == "second");
			CHECK(events[2].Request.Id.is_null());
			CHECK(events[3].Request.Id.is_null());
		}

		TEST_CASE("Dispatcher: pending errors and malformed notification results report failure")
		{
			for (uint32_t mode : { 0U, 1U })
			{
				for (bool notification : { false, true })
				{
					std::vector<DispatcherRequestEvent> events;
					DispatcherSetup setup({ .RequestObserver = [&events](const DispatcherRequestEvent& event)
					{
						events.push_back(event);
					} });
					RpcRequest request = Test::MakeTestRequest(1, "test.observedOutcome", Json{ { "polls", mode } });
					request.IsNotification = notification;
					if (notification)
						request.Id = Json();
					setup.Calls->Enqueue(1, std::move(request));
					const auto messages = setup.PumpAll();
					CHECK(messages.size() == (notification ? 0U : 1U));
					REQUIRE(events.size() == 2);
					CHECK(events[0].Phase == DispatcherRequestPhase::Started);
					CHECK(events[1].Phase == DispatcherRequestPhase::Failed);
					CHECK(events[1].Sequence == events[0].Sequence);
					const auto calls = setup.CallsOf("test.observedOutcome");
					CHECK(std::count(calls.begin(), calls.end(), "finish") == 1);
				}
			}
		}

		TEST_CASE("Dispatcher: offload failures remain failures when the client disconnects during output")
		{
			for (bool disconnect : { false, true })
			{
				std::vector<DispatcherRequestEvent> events;
				DispatcherSetup setup({ .OffloadThresholdBytes = 16,
					.RequestObserver = [&events](const DispatcherRequestEvent& event)
				{
					events.push_back(event);
				} });
				setup.Host.FailOffload = true;
				if (disconnect)
					setup.Host.BeforeOffload = [&setup]()
					{
						setup.Calls->RemoveClient(1);
					};
				setup.Calls->Enqueue(1, Test::MakeTestRequest(1, "test.large", Json{ { "count", 3 } }));
				const auto messages = setup.PumpAll();
				CHECK(messages.size() == (disconnect ? 0U : 1U));
				if (!disconnect)
					CHECK(messages[0].Message["error"]["code"] == Json(std::to_underlying(RpcErrorCode::Internal)));
				REQUIRE(events.size() == 2);
				CHECK(events.back().Phase == DispatcherRequestPhase::Failed);
				CHECK(setup.Host.OffloadAttempts == 1);
			}
		}

		TEST_CASE("Dispatcher: disconnects during invocation retain explicit completion or cancellation")
		{
			for (const std::string method : { "test.fail", "test.read", "test.pend" })
			{
				std::vector<DispatcherRequestEvent> events;
				DispatcherSetup setup({ .RequestObserver = [&events](const DispatcherRequestEvent& event)
				{
					events.push_back(event);
				} });
				setup.Host.AfterInvocation = [&setup]()
				{
					setup.Calls->RemoveClient(1);
				};
				setup.Calls->Enqueue(1, Test::MakeTestRequest(1, method, method == "test.pend" ? Json{ { "polls", 1000 } } : Json::object()));
				CHECK(setup.PumpAll().empty());
				REQUIRE(events.size() == 2);
				CHECK(events[1].Phase == (method == "test.fail" ? DispatcherRequestPhase::Failed : method == "test.read" ? DispatcherRequestPhase::Succeeded
																														 : DispatcherRequestPhase::Cancelled));
				const auto calls = setup.CallsOf(method);
				CHECK(std::count(calls.begin(), calls.end(), "finish") == 1);
			}
		}

		TEST_CASE("Dispatcher: a disconnect during Poll emits one terminal event after finishing the request")
		{
			for (const bool resolves : { false, true })
			{
				std::vector<DispatcherRequestEvent> events;
				DispatcherSetup setup({ .RequestObserver = [&events](const DispatcherRequestEvent& event)
				{
					events.push_back(event);
				} });
				setup.Calls->Enqueue(1, Test::MakeTestRequest(1, "test.pend", Json{ { "polls", resolves ? 2 : 1000 } }));
				CHECK(setup.Calls->Pump(std::chrono::seconds(1)).empty());
				REQUIRE(events.size() == 1);
				setup.Host.AfterInvocation = [&setup]()
				{
					setup.Calls->RemoveClient(1);
				};
				CHECK(setup.PumpAll().empty());
				REQUIRE(events.size() == 2);
				CHECK(events[1].Sequence == events[0].Sequence);
				CHECK(events[1].Phase == (resolves ? DispatcherRequestPhase::Succeeded : DispatcherRequestPhase::Cancelled));
				const auto calls = setup.CallsOf("test.pend");
				CHECK(std::count(calls.begin(), calls.end(), "finish") == 1);
				CHECK(std::count(calls.begin(), calls.end(), "cancel") == (resolves ? 0 : 1));
			}
		}

		TEST_CASE("Dispatcher: a completed offload retains success without responding to a disconnected client")
		{
			std::vector<DispatcherRequestEvent> events;
			DispatcherSetup setup({ .OffloadThresholdBytes = 16,
				.RequestObserver = [&events](const DispatcherRequestEvent& event)
			{
				events.push_back(event);
			} });
			setup.Host.BeforeOffload = [&setup]()
			{
				setup.Calls->RemoveClient(1);
			};
			setup.Calls->Enqueue(1, Test::MakeTestRequest(1, "test.large", Json{ { "count", 3 } }));
			CHECK(setup.PumpAll().empty());
			REQUIRE(events.size() == 2);
			CHECK(events[1].Phase == DispatcherRequestPhase::Succeeded);
			CHECK(events[1].Sequence == events[0].Sequence);
			CHECK(setup.Host.OffloadAttempts == 1);
		}

		TEST_CASE("Dispatcher: requests of one client run in order and every response carries _meta")
		{
			DispatcherSetup setup;
			setup.State.Revision = 7;
			setup.Calls->Enqueue(1, Test::MakeTestRequest(1, "test.echo", Json{ { "text", "a" } }));
			setup.Calls->Enqueue(1, Test::MakeTestRequest(2, "test.read", Json::object()));
			std::vector<OutboundMessage> messages = setup.PumpAll();
			REQUIRE(messages.size() == 2);
			CHECK(messages[0].Client == 1);
			CHECK(messages[0].Message["id"] == Json(1));
			CHECK(messages[0].Message["result"]["text"] == Json("a"));
			CHECK(messages[0].Message["result"]["_meta"]["revision"] == Json(7));
			CHECK(messages[1].Message["id"] == Json(2));
			CHECK(messages[1].Message["result"]["revision"] == Json(7));
			const std::vector<std::string> calls = { "available test.echo", "admit test.echo", "enter test.echo", "handler test.echo",
				"leave test.echo", "finish test.echo", "available test.read", "admit test.read", "enter test.read", "handler test.read",
				"leave test.read", "finish test.read" };
			CHECK(setup.State.Calls == calls);
		}

		TEST_CASE("Dispatcher: availability is checked before the params are read")
		{
			DispatcherSetup setup;
			setup.State.LauncherState = true;
			setup.Calls->Enqueue(1, Test::MakeTestRequest(1, "test.echo", Json::object())); // its required "text" is missing
			setup.Calls->Enqueue(1, Test::MakeTestRequest(2, "test.read", Json::object()));
			std::vector<OutboundMessage> messages = setup.PumpAll();
			REQUIRE(messages.size() == 2);
			CHECK(messages[0].Message["error"]["code"] == Json(-32002)); // InvalidState, not InvalidParams
			CHECK(messages[0].Message["error"]["data"]["detail"].dump().contains("no project open"));
			CHECK(setup.CallsOf("test.echo") == std::vector<std::string>{ "available" }); // nothing after the refusal
			CHECK(messages[1].Message.contains("result"));
		}

		TEST_CASE("Dispatcher: a pending operation is admitted once and every Poll runs between enter and leave")
		{
			DispatcherSetup setup;
			setup.Calls->Enqueue(1, Test::MakeTestRequest(1, "test.pend", Json{ { "polls", 2 } }));
			std::vector<OutboundMessage> messages = setup.PumpAll();
			REQUIRE(messages.size() == 1);
			CHECK(messages[0].Message["result"]["polls"] == Json(2));
			const std::vector<std::string> calls = { "available", "admit", "enter", "leave", "enter", "leave", "enter", "leave", "finish" };
			CHECK(setup.CallsOf("test.pend") == calls);
		}

		TEST_CASE("Dispatcher: Cancel runs on disconnect even while the host refuses new requests")
		{
			DispatcherSetup setup;
			setup.Calls->Enqueue(1, Test::MakeTestRequest(1, "test.pend", Json{ { "polls", 1000 } }));
			CHECK(setup.Calls->Pump(std::chrono::microseconds(1000000)).empty());
			REQUIRE(setup.Calls->GetPendingCount(1) == 1);

			setup.State.DenyEverything = true; // a stale ifRevision or a read-only switch would refuse an admission now
			setup.Calls->RemoveClient(1);
			// The Cancel runs between enter and leave, and FinishRequest closes the request; it is never admitted again.
			const std::vector<std::string> calls = setup.CallsOf("test.pend");
			REQUIRE(calls.size() >= 4);
			const std::vector<std::string> last(calls.end() - 4, calls.end());
			CHECK(last == std::vector<std::string>{ "enter", "cancel", "leave", "finish" });
			CHECK(std::count(calls.begin(), calls.end(), "admit") == 1);
		}

		TEST_CASE("Dispatcher: a pending operation is polled once per pump and holds back its client's next request")
		{
			DispatcherSetup setup;
			setup.Calls->Enqueue(1, Test::MakeTestRequest(1, "test.pend", Json{ { "polls", 3 } }));
			setup.Calls->Enqueue(1, Test::MakeTestRequest(2, "test.read", Json::object()));
			setup.Calls->Enqueue(2, Test::MakeTestRequest(3, "test.read", Json::object()));

			std::vector<OutboundMessage> first = setup.Calls->Pump(std::chrono::microseconds(1000000));
			REQUIRE(first.size() == 1); // client 2 runs while client 1 waits for its operation
			CHECK(first[0].Client == 2);
			CHECK(setup.Calls->GetPendingCount(1) == 1);
			CHECK(setup.Calls->GetQueuedCount(1) == 1);

			CHECK(setup.Calls->Pump(std::chrono::microseconds(1000000)).empty());
			std::vector<OutboundMessage> third = setup.Calls->Pump(std::chrono::microseconds(1000000));
			REQUIRE_FALSE(third.empty());
			CHECK(third[0].Message["id"] == Json(1));
			CHECK(third[0].Message["result"]["polls"] == Json(3));
			std::vector<OutboundMessage> rest = setup.PumpAll();
			CHECK((third.size() == 2 || rest.size() == 1));
		}

		TEST_CASE("Dispatcher: removing a client cancels its pending operations without a response")
		{
			DispatcherSetup setup;
			setup.Calls->Enqueue(1, Test::MakeTestRequest(1, "test.pend", Json{ { "polls", 1000 } }));
			setup.Calls->Enqueue(1, Test::MakeTestRequest(2, "test.read", Json::object()));
			CHECK(setup.Calls->Pump(std::chrono::microseconds(1000000)).empty());
			REQUIRE(setup.Calls->GetPendingCount(1) == 1);

			setup.Calls->RemoveClient(1);
			CHECK(setup.Calls->GetPendingCount(1) == 0);
			CHECK(setup.Calls->GetQueuedCount(1) == 0);
			CHECK_FALSE(setup.Calls->HasWork());
			CHECK(std::find(setup.State.Calls.begin(), setup.State.Calls.end(), "cancel test.pend") != setup.State.Calls.end());
			CHECK(setup.PumpAll().empty());
		}

		TEST_CASE("Dispatcher: an exception escaping a handler asserts in Debug builds and is Internal otherwise")
		{
#if defined(ENGINE_DEBUG)
			ENGINE_CHECK_DEATH("Automation/DispatcherHandlerExceptionAssertsInDebug", "test.throw");
#else
			DispatcherSetup setup;
			setup.Calls->Enqueue(1, Test::MakeTestRequest(1, "test.throw", Json::object()));
			setup.Calls->Enqueue(1, Test::MakeTestRequest(2, "test.read", Json::object()));
			std::vector<OutboundMessage> messages = setup.PumpAll();
			REQUIRE(messages.size() == 2);
			CHECK(messages[0].Message["error"]["code"] == Json(-32000));
			CHECK(messages[1].Message.contains("result")); // the dispatcher keeps serving
#endif
		}

		TEST_CASE("Dispatcher: a std::system_error escaping a handler goes to the host's SystemErrorHandler first")
		{
			// vulkan.hpp's vk::SystemError is a std::system_error; the editor's handler ends the process like the
			// frame-boundary catch (Docs/Decisions/0009-m5-decisions.md decisions 5 and 33).
			ENGINE_CHECK_DEATH("Automation/DispatcherSystemErrorReachesTheHostHandler", "the host saw a std::system_error escape 'test.systemError'");
		}

		TEST_CASE("Dispatcher: after a SystemErrorHandler that returns, a system error is handled like any other exception")
		{
#if defined(ENGINE_DEBUG)
			ENGINE_CHECK_DEATH("Automation/DispatcherReturningSystemErrorHandlerAsserts", "test.systemError");
#else
			int handled = 0;
			int* count = &handled;
			std::string method;
			std::string* seen = &method;
			DispatcherSetup setup(DispatcherSpecification{ .SystemErrors = [count, seen](const std::system_error& /*error*/, std::string_view name) -> std::optional<Error>
			{
				++*count;
				*seen = std::string(name);
				return std::nullopt;
			} });
			setup.Calls->Enqueue(1, Test::MakeTestRequest(1, "test.systemError", Json::object()));
			setup.Calls->Enqueue(1, Test::MakeTestRequest(2, "test.read", Json::object()));
			std::vector<OutboundMessage> messages = setup.PumpAll();
			REQUIRE(messages.size() == 2);
			CHECK(handled == 1);
			CHECK(method == "test.systemError");
			CHECK(messages[0].Message["error"]["code"] == Json(-32000));
			CHECK(messages[1].Message.contains("result")); // the dispatcher keeps serving
#endif
		}

		TEST_CASE("Dispatcher: a SystemErrorHandler's error answers the request, and the dispatcher keeps serving")
		{
			// M18 (C18.3, Docs/Decisions/0029-m18-contract.md decision 28): a host that handled the exception (a device loss,
			// recovered at the frame boundary) answers with its own error, in every configuration, without the assert.
			int handled = 0;
			int* count = &handled;
			DispatcherSetup setup(DispatcherSpecification{ .SystemErrors = [count](const std::system_error& /*error*/, std::string_view name) -> std::optional<Error>
			{
				++*count;
				return Error(ErrorCode::Gpu, std::format("the GPU device was lost during '{}'", name));
			} });
			setup.Calls->Enqueue(1, Test::MakeTestRequest(1, "test.systemError", Json::object()));
			setup.Calls->Enqueue(1, Test::MakeTestRequest(2, "test.read", Json::object()));
			std::vector<OutboundMessage> messages = setup.PumpAll();
			REQUIRE(messages.size() == 2);
			CHECK(handled == 1);
			CHECK(messages[0].Message["error"]["data"]["errorCode"] == Json("Gpu"));
			CHECK(messages[0].Message["error"]["data"]["detail"].dump().contains("test.systemError"));
			CHECK(messages[1].Message.contains("result"));
		}

		TEST_CASE("Dispatcher: unknown methods, invalid params and host refusals are error responses")
		{
			DispatcherSetup setup;
			setup.Calls->Enqueue(1, Test::MakeTestRequest(1, "test.ecko", Json::object()));
			setup.Calls->Enqueue(1, Test::MakeTestRequest(2, "test.echo", Json{ { "count", 2 } }));
			setup.Calls->Enqueue(1, Test::MakeTestRequest(3, "test.read", Json{ { "dryRun", true } }));
			setup.Calls->Enqueue(1, Test::MakeTestRequest(4, "test.fail", Json::object()));
			std::vector<OutboundMessage> messages = setup.PumpAll();
			REQUIRE(messages.size() == 4);
			CHECK(messages[0].Message["error"]["code"] == Json(-32601));
			CHECK(messages[0].Message["error"]["data"]["issues"][0]["hint"].dump().contains("test.echo"));
			CHECK(messages[1].Message["error"]["code"] == Json(-32602));
			CHECK(messages[2].Message["error"]["code"] == Json(-32009));
			CHECK(messages[3].Message["error"]["code"] == Json(-32001));
			CHECK(messages[3].Message["error"]["data"].contains("_meta"));

			setup.State.DenyEverything = true;
			setup.State.Calls.clear();
			setup.Calls->Enqueue(2, Test::MakeTestRequest(5, "test.read", Json::object()));
			std::vector<OutboundMessage> denied = setup.PumpAll();
			REQUIRE(denied.size() == 1);
			CHECK(denied[0].Message["error"]["code"] == Json(-32008));
			// A refused request never reaches its handler, and FinishRequest still runs.
			const std::vector<std::string> calls = { "available", "admit", "finish" };
			CHECK(setup.CallsOf("test.read") == calls);
		}

		TEST_CASE("Dispatcher: results over the threshold are offloaded through the host")
		{
			DispatcherSetup setup(DispatcherSpecification{ .OffloadThresholdBytes = 1024 });
			setup.Calls->Enqueue(1, Test::MakeTestRequest(1, "test.large", Json{ { "count", 100 } }));
			setup.Calls->Enqueue(1, Test::MakeTestRequest(2, "test.large", Json{ { "count", 1 } }));
			std::vector<OutboundMessage> messages = setup.PumpAll();
			REQUIRE(messages.size() == 2);
			Json& offloaded = messages[0].Message["result"];
			CHECK(offloaded["truncated"] == Json(true));
			CHECK(offloaded["path"] == Json(std::format("/offload/{}-00000001.json", Test::TestOffloadServerTag)));
			CHECK(offloaded["summary"]["items"]["count"] == Json(100));
			CHECK(offloaded.contains("_meta"));
			REQUIRE(setup.State.Offloaded.size() == 1);
			CHECK(setup.State.Offloaded[0].second.contains("\"items\""));
			CHECK_FALSE(messages[1].Message["result"].contains("truncated"));
		}

		TEST_CASE("Dispatcher: a client added without offloading keeps large results inline")
		{
			DispatcherSetup setup(DispatcherSpecification{ .OffloadThresholdBytes = 1024 });
			setup.Calls->AddClient(3, "batch", { .OffloadLargeResults = false });
			setup.Calls->Enqueue(3, Test::MakeTestRequest(1, "test.large", Json{ { "count", 100 } }));
			std::vector<OutboundMessage> messages = setup.PumpAll();
			REQUIRE(messages.size() == 1);
			CHECK_FALSE(messages[0].Message["result"].contains("truncated"));
			CHECK(messages[0].Message["result"]["items"].size() == 100);
			CHECK(setup.State.Offloaded.empty());
		}

		TEST_CASE("Dispatcher: dry runs carry dryRun and notifications get no response")
		{
			DispatcherSetup setup;
			setup.Calls->Enqueue(1, Test::MakeTestRequest(1, "test.echo", Json{ { "text", "a" }, { "dryRun", true } }));
			RpcRequest notification = Test::MakeTestRequest(0, "test.read", Json::object());
			notification.Id = Json();
			notification.IsNotification = true;
			setup.Calls->Enqueue(1, std::move(notification));
			std::vector<OutboundMessage> messages = setup.PumpAll();
			REQUIRE(messages.size() == 1);
			CHECK(messages[0].Message["result"]["dryRun"] == Json(true));
			CHECK(std::count(setup.State.Calls.begin(), setup.State.Calls.end(), "handler test.read") == 1);
		}

		// World Tree brief 08: a pending method may support dry runs (asset.import {replace: true}) whose sandbox must not outlast
		// the request's stretch, so its dry run resolves within the one pump that ran it: an operation that answers at its first
		// poll is the dry run's result, and one that would need another poll is cancelled and fails, never left pending.
		TEST_CASE("Dispatcher: a pending method's dry run resolves within its pump or is cancelled")
		{
			DispatcherSetup setup;
			setup.Calls->Enqueue(1, Test::MakeTestRequest(1, "test.pend", Json{ { "polls", 1 }, { "dryRun", true } }));
			std::vector<OutboundMessage> answered = setup.Calls->Pump(std::chrono::seconds(1));
			REQUIRE(answered.size() == 1);
			CHECK(answered[0].Message["result"]["polls"] == Json(1));
			CHECK(answered[0].Message["result"]["dryRun"] == Json(true));
			CHECK(setup.Calls->GetPendingCount(1) == 0);

			setup.Calls->Enqueue(1, Test::MakeTestRequest(2, "test.pend", Json{ { "polls", 2 }, { "dryRun", true } }));
			std::vector<OutboundMessage> refused = setup.Calls->Pump(std::chrono::seconds(1));
			REQUIRE(refused.size() == 1);
			CHECK(refused[0].Message["error"]["data"]["errorCode"] == Json("Cancelled"));
			CHECK(refused[0].Message["error"]["message"].dump().contains("did not resolve its dry run at once"));
			CHECK(setup.Calls->GetPendingCount(1) == 0);
			CHECK(std::count(setup.State.Calls.begin(), setup.State.Calls.end(), "cancel test.pend") == 1);
			const std::vector<std::string> calls = setup.CallsOf("test.pend");
			CHECK(std::count(calls.begin(), calls.end(), "finish") == 2);

			// Without dryRun the same operation stays pending until its second poll.
			setup.Calls->Enqueue(1, Test::MakeTestRequest(3, "test.pend", Json{ { "polls", 2 } }));
			CHECK(setup.Calls->Pump(std::chrono::seconds(1)).empty());
			CHECK(setup.Calls->GetPendingCount(1) == 1);
			REQUIRE(setup.PumpAll().size() == 1);
		}

		// §13.2 "Progress" (Skerries' brief S20): a long operation's progress reaches the client that asked for it, so the
		// client bounds the call by whether its work advances rather than by a fixed deadline.
		TEST_CASE("Dispatcher: a pending operation's progress reaches its client at once, then per interval or new stage")
		{
			ManualClock clock;
			DispatcherSetup setup(DispatcherSpecification{ .ProgressInterval = std::chrono::milliseconds(1000), .TimeSource = clock.GetFunction() });
			setup.Calls->AddClient(3, "agent", { .OffloadLargeResults = true, .Progress = true });
			const auto pump = [&setup]()
			{
				return setup.Calls->Pump(std::chrono::seconds(1));
			};
			setup.Calls->Enqueue(3, Test::MakeTestRequest(7, "test.progress", Json{ { "polls", 6 } }));

			// The handler returned it: one notification before its first Poll, with nothing done yet; that Poll sends none.
			std::vector<OutboundMessage> first = pump();
			REQUIRE(first.size() == 1);
			CHECK(first[0].Client == 3);
			const Json& notification = first[0].Message;
			CHECK(notification["jsonrpc"] == Json("2.0"));
			CHECK(notification["method"] == Json("$/progress"));
			CHECK_FALSE(notification.contains("id"));
			const Json& params = notification["params"];
			CHECK(params["id"] == Json(7));
			CHECK(params["method"] == Json("test.progress"));
			CHECK(params["work"] == Json(0));
			CHECK(params["stage"] == Json("suite"));
			CHECK(params["subject"] == Json("Board/clears a line"));
			CHECK((params["tick"] == Json(0) && params["finalTick"] == Json(0)));
			CHECK((params["step"] == Json(1) && params["steps"] == Json(2)));
			CHECK(params["message"] == Json(DescribePendingProgress({ .Work = 0, .Stage = PendingProgressStage::Suite, .Subject = "Board/clears a line", .Tick = 0, .FinalTick = 0, .Step = 1, .Steps = 2 })));

			// Within the interval, the same stage sends nothing; once it has passed, the next Poll sends.
			clock.Now += std::chrono::milliseconds(999);
			CHECK(pump().empty());
			clock.Now += std::chrono::milliseconds(1);
			std::vector<OutboundMessage> interval = pump();
			REQUIRE(interval.size() == 1);
			CHECK(interval[0].Message["params"]["work"] == Json(3));
			CHECK(interval[0].Message["params"]["stage"] == Json("suite"));

			// A new stage is sent at once; the same stage again only after the interval; then the response, with no
			// notification after it.
			std::vector<OutboundMessage> stage = pump();
			REQUIRE(stage.size() == 1);
			CHECK(stage[0].Message["params"]["work"] == Json(4));
			CHECK(stage[0].Message["params"]["stage"] == Json("replay"));
			CHECK(pump().empty());
			std::vector<OutboundMessage> last = pump();
			REQUIRE(last.size() == 1);
			CHECK(last[0].Message["id"] == Json(7));
			CHECK(last[0].Message["result"]["polls"] == Json(6));
		}

		TEST_CASE("Dispatcher: a new stage is sent after the Poll that reached it, whatever the interval")
		{
			ManualClock clock;
			DispatcherSetup setup(DispatcherSpecification{ .ProgressInterval = std::chrono::milliseconds(1000), .TimeSource = clock.GetFunction() });
			setup.Calls->AddClient(3, "agent", { .OffloadLargeResults = true, .Progress = true });
			setup.Calls->Enqueue(3, Test::MakeTestRequest(8, "test.progress", Json{ { "polls", 6 } }));
			std::vector<std::string> stages;
			std::vector<uint64_t> work;
			bool answered = false;
			for (int pump = 0; pump < 10 && !answered; ++pump)
			{
				for (const OutboundMessage& message : setup.Calls->Pump(std::chrono::seconds(1)))
				{
					if (message.Message.contains("id"))
					{
						answered = true;
						continue;
					}
					stages.push_back(JsonReader(message.Message["params"]["stage"]).ReadString().value_or(std::string()));
					work.push_back(JsonReader(message.Message["params"]["work"]).ReadUInt64().value_or(0));
				}
			}
			CHECK(answered);
			// The clock never moved: the first notification, then the Poll that turned the stage.
			CHECK(stages == std::vector<std::string>{ "suite", "replay" });
			CHECK(work == std::vector<uint64_t>{ 0, 4 });
		}

		TEST_CASE("Dispatcher: progress reaches only a client that asked for it, from an operation that reports it")
		{
			DispatcherSetup setup(DispatcherSpecification{ .ProgressInterval = std::chrono::milliseconds(0) });
			setup.Calls->AddClient(3, "agent", { .OffloadLargeResults = true, .Progress = true });
			// Client 1 asked for none.
			setup.Calls->Enqueue(1, Test::MakeTestRequest(1, "test.progress", Json{ { "polls", 3 } }));
			// test.pend reports none.
			setup.Calls->Enqueue(3, Test::MakeTestRequest(2, "test.pend", Json{ { "polls", 3 } }));
			std::vector<OutboundMessage> messages = setup.PumpAll();
			REQUIRE(messages.size() == 2);
			for (const OutboundMessage& message : messages)
				CHECK(message.Message.contains("id"));

			// With a zero interval, every Poll that leaves it running is followed by one.
			setup.Calls->Enqueue(3, Test::MakeTestRequest(3, "test.progress", Json{ { "polls", 3 } }));
			std::vector<uint64_t> work;
			for (const OutboundMessage& message : setup.PumpAll())
			{
				if (!message.Message.contains("id"))
					work.push_back(JsonReader(message.Message["params"]["work"]).ReadUInt64().value_or(0));
			}
			CHECK(work == std::vector<uint64_t>{ 0, 1, 2 });

			// A notification request gets neither a response nor progress.
			setup.Calls->Enqueue(3, RpcRequest{ .Id = Json(), .IsNotification = true, .Method = "test.progress", .Params = Json{ { "polls", 3 } }, .TranscriptLine = std::nullopt });
			CHECK(setup.PumpAll().empty());
			CHECK(setup.Calls->GetPendingCount(3) == 0);
		}

		// Skerries' brief S20: a Busy answer names the request whose handler, Poll or Cancel holds the main thread.
		TEST_CASE("Dispatcher: the watchdog names the request whose handler, Poll or Cancel runs, and none between them")
		{
			Watchdog watchdog(DefaultWatchdogStallThreshold, std::chrono::steady_clock::now());
			DispatcherSetup setup({}, &watchdog);
			setup.Calls->Enqueue(1, Test::MakeTestRequest(1, "test.observeRequest", Json::object()));
			CHECK(setup.Calls->Pump(std::chrono::seconds(1)).empty()); // the handler and the first Poll
			CHECK(watchdog.GetRequest().empty());
			CHECK(setup.Calls->Pump(std::chrono::seconds(1)).empty()); // the second Poll
			CHECK(watchdog.GetRequest().empty());
			setup.Calls->RemoveClient(1); // Cancel
			CHECK(watchdog.GetRequest().empty());
			const std::vector<std::string> observed = setup.CallsOf("test.observeRequest");
			CHECK(std::count(observed.begin(), observed.end(), "request") == 4);
		}
	}

}
