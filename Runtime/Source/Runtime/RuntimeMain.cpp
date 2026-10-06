#include "Engine/App/EntryPoint.h"
#include "Runtime/RuntimeApp.h"

int main(int argc, char** argv)
{
	return Engine::RunApplication(argc, argv, &Engine::CreateRuntimeApp);
}
