#include "EditorPCH.h"

#include "Editor/EditorApp.h"
#include "Engine/App/EntryPoint.h"

int main(int argc, char** argv)
{
	return Engine::RunApplication(argc, argv, &Engine::CreateEditorApp);
}
