// Node-API functions are imported from "node.exe", but in ORY-1 they live in the Electron executable that loaded the
// addon. The import is delay-loaded, and this hook points it at whichever executable is running (the same trick node-gyp uses).
#define NOMINMAX
#include <windows.h>
#include <delayimp.h>
#include <string.h>

static FARPROC WINAPI loadExeHook (unsigned int event, DelayLoadInfo* info)
{
	if (event != dliNotePreLoadLibrary || _stricmp (info->szDll, "node.exe") != 0) return nullptr;
	return (FARPROC)GetModuleHandle (nullptr);
}

extern "C" const PfnDliHook __pfnDliNotifyHook2 = loadExeHook;
