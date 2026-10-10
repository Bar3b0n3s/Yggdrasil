#include "EnginePCH.h"
#include "Engine/Scripting/TaskScheduler.h"

namespace Engine {

	struct TaskScheduler::State
	{
	};

	TaskScheduler::TaskScheduler(ScriptEngine& /*engine*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	TaskScheduler::~TaskScheduler() = default;

	Result<ScriptReference> TaskScheduler::Spawn(ScriptReference /*function*/, ScriptTaskOwner /*owner*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "Task spawning is an M13 contract stub");
	}

	Result<ScriptReference> TaskScheduler::Delay(double /*seconds*/, ScriptReference /*function*/, ScriptTaskOwner /*owner*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "Task delay is an M13 contract stub");
	}

	Status TaskScheduler::Cancel(ScriptReference /*task*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "Task cancellation is an M13 contract stub");
	}

	void TaskScheduler::CancelEntity(UUID /*entity*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	void TaskScheduler::CancelCase(ScriptReference /*caseThread*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	void TaskScheduler::CancelAll()
	{
		ENGINE_CONTRACT_STUB();
	}

	void TaskScheduler::ResumeDue()
	{
		ENGINE_CONTRACT_STUB();
	}

	size_t TaskScheduler::GetCount() const
	{
		ENGINE_CONTRACT_STUB();
		return 0;
	}

}
