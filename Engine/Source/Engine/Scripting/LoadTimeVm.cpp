#include "EnginePCH.h"
#include "Engine/Scripting/LoadTimeVm.h"

#include "Engine/Asset/IScriptDiagnosticsProvider.h"
#include "Engine/Asset/ScriptData.h"
#include "Engine/Scripting/Private/BindingRegistration.h"
#include "Engine/Scripting/Private/SandboxAccess.h"
#include "Engine/Scripting/RequireResolver.h"
#include "Engine/Scripting/ScriptCompiler.h"

namespace Engine {

	Result<Ref<const ScriptData>> LoadTimeVm::Extract(const ScriptCheckRequest& request, const LoadTimeVmSpecification& specification)
	{
#if defined(ENGINE_DIST)
		static_cast<void>(request);
		static_cast<void>(specification);
		return MakeError(ErrorCode::Unsupported, "load-time script extraction is unavailable in Dist");
#else
		if (!request.Modules)
			return MakeError(ErrorCode::InvalidArgument, "script extraction requires a module reader");
		if (request.Path.GetScheme() != "project" || !request.Path.GetPath().starts_with("Assets/") || !request.Path.GetPath().ends_with(".luau"))
			return MakeError(ErrorCode::Validation, "script extraction requires a project Assets .luau origin");
		SandboxSpecification sandboxSpecification{};
		sandboxSpecification.Mode = SandboxMode::LoadTime;
		sandboxSpecification.MemoryLimitMB = specification.MemoryLimitMB;
		sandboxSpecification.SourceModules = request.Modules;
		sandboxSpecification.ClockSeconds = specification.ClockSeconds;
		ENGINE_TRY_ASSIGN(auto sandbox, Sandbox::Create(sandboxSpecification));
		ENGINE_TRY(Detail::SandboxAccess::EnterProtected(*sandbox, ScriptExecutionOrigin::Pure));
		struct GraphScope
		{
			Sandbox& Vm;
			~GraphScope() { Detail::SandboxAccess::LeaveProtected(Vm); }
		} scope{ *sandbox };
		ENGINE_TRY_ASSIGN(auto compilation, ScriptCompiler::Compile({ .Path = request.Path, .Source = request.Source }));
		auto data = CreateRef<ScriptData>();
		data->Bytecode = std::move(compilation.Bytecode);
		data->SourceMap = std::move(compilation.SourceMap);
		ENGINE_TRY(sandbox->RunModule(*data));
		Status extracted{};
		const auto outcome = Detail::SandboxAccess::RunNative(*sandbox, [&data, &request, &extracted](ScriptCall& call)
		{
			extracted = Detail::SandboxAccess::PushModuleReturn(call, request.Path);
			if (!extracted)
				return 0;
			const auto registration = Detail::InspectScriptRegistration(call, -1);
			if (!registration)
			{
				extracted = std::unexpected(registration.error());
				return 0;
			}
			data->Kind = registration->Kind;
			data->Name = registration->Name;
			data->Fields = registration->Fields;
			return 0;
		});
		if (!outcome)
			return std::unexpected(outcome.error());
		if (outcome->Failure)
		{
			const auto& failure = *outcome->Failure;
			ErrorLocation location{};
			location.File = failure.Script;
			location.Line = failure.Line;
			location.Column = failure.Column;
			if (!failure.JsonPointer.empty())
				location.JsonPointer = failure.JsonPointer;
			return std::unexpected(Error(failure.Kind == ScriptErrorKind::Timeout ? ErrorCode::Timeout : ErrorCode::Script, failure.Message).WithLocation(std::move(location)));
		}
		if (!extracted)
		{
			ErrorLocation location{};
			location.File = request.Path.GetPath();
			return std::unexpected(std::move(extracted.error()).WithLocation(std::move(location)));
		}
		const auto edges = Detail::SandboxAccess::GetResolver(*sandbox)->GetRequires();
		data->Requires.assign(edges.begin(), edges.end());
		return Ref<const ScriptData>(std::move(data));
#endif
	}

}
