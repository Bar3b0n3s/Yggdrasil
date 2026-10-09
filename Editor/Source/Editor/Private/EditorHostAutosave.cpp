#include "EditorPCH.h"
#include "Editor/Private/EditorHostAutosave.h"

#include "EditorCore/Autosave/Autosave.h"

namespace Engine {

	FatalErrorHook MakeEditorAutosaveFatalHook(Autosave& saves)
	{
		return { .Function = [](void* userData, FatalErrorKind /*kind*/, std::string_view /*message*/)
		{
			(void)static_cast<Autosave*>(userData)->WriteFatalSnapshot();
		},
			.UserData = &saves };
	}

}
