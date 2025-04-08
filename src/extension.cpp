#include "extension.h"
#include "debugger.h"
#include <fmt/format.h>
#include <string>
#include <thread>

#define LOWEST_SOURCEPAWN_API_VERSION 0x0207
Extension g_zr;
SMEXT_LINK(&g_zr);

#ifndef _WIN32
#define GetProcAddress dlsym
// Linux doesn't have this function so this emulates its functionality
void* GetModuleHandle(const char* name)
{
    std::string path;
    const char* modPath = g_pSM->GetSourceModPath();
    if (modPath) {
        path = std::string(modPath) + "/bin/" + std::string(name);
    }
#define HMODULE void*
    void* handle;
    if (name == nullptr) {
        // hmm, how can this be handled under linux....
        // is it even needed?
        return nullptr;
    }

    if ((handle = dlopen(path.c_str(), RTLD_NOW)) == nullptr) {
        // printf("Error:%s\n",dlerror());
        // couldn't open this file
        return nullptr;
    }

    // read "man dlopen" for details
    // in short dlopen() inc a ref count
    // so dec the ref count by performing the close
    dlclose(handle);
    return handle;
}
#endif // _WIN32
extern void(DebugHandler)(SourcePawn::IPluginContext* IPlugin,
    sp_debug_break_info_t& BreakOnfo,
    const SourcePawn::IErrorReport* IErrorReport);

extern void debugThread();
bool Inited = false;

extern DebugReport DebugListener;

uint16_t sm_debugger_port = 3000; // Default port value
float sm_debugger_delay = 0.f; // Default delay value
int SM_Debugger_port()
{
    return sm_debugger_port;
}
float SM_Debugger_timeout()
{
    return sm_debugger_delay;
}
/*

bool Extension::SDK_OnMetamodLoad(ISmmAPI* ismm, char* error, size_t maxlen,
bool late) { GET_V_IFACE_CURRENT(GetEngineFactory, icvar, ICvar,
CVAR_INTERFACE_VERSION); #if SOURCE_ENGINE >= SE_ORANGEBOX g_pCVar = icvar;
        ConVar_Register(0, this);
#else
        ConCommandBaseMgr::OneTimeInit(this);
#endif

        return true;
}
*/

bool Extension::SDK_OnLoad(char* error, size_t maxlen, bool late)
{
    if (late) {
        snprintf(error, maxlen,
            "Debugger breakpoints works only before any plugins loaded. "
            "(create file sm_debugger.autoload in extensions folder)");
        return false;
    }
    ISourcePawnFactory* factory = nullptr;
    GetSourcePawnFactoryFn factoryFn = nullptr;
    ISourcePawnEnvironment* current_env = nullptr;

    // Try to load x64 first, then fall back to x86 if not found
    std::string modulename_x64 = "sourcepawn.jit.x64." PLATFORM_LIB_EXT;
    std::string modulename_x86 = "sourcepawn.jit.x86." PLATFORM_LIB_EXT;

    const char* debugPort = g_pSM->GetCoreConfigValue("DebuggerPort");
    const char* debugDelay = g_pSM->GetCoreConfigValue("DebuggerWaitTime");

    // Parse debugger port from config or use default
    if (debugPort && debugPort[0]) {
        try {
            sm_debugger_port = std::stoi(debugPort);
            fmt::print("[SM_DEBUGGER] Using port: {}\n", sm_debugger_port);
        } catch (std::invalid_argument& e) {
            fmt::print(
                "Can't convert DebuggerPort from core.cfg. Invalid argument: [%s]\n",
                debugPort);
            fmt::print("[SM_DEBUGGER] Using default port: %d\n", sm_debugger_port);
        } catch (std::out_of_range& e) {
            fmt::print(
                "Can't convert DebuggerPort from core.cfg. unsigned short is out of "
                "range! [%s]\n",
                debugPort);
            fmt::print("[SM_DEBUGGER] Using default port: %d\n", sm_debugger_port);
        } catch (...) {
            fmt::print(
                "Can't convert DebuggerPort from core.cfg. unknown problem! [%s]\n",
                debugPort);
            fmt::print("[SM_DEBUGGER] Using default port: %d\n", sm_debugger_port);
        }
    }

    // Parse debugger delay from config or use default
    if (debugDelay && debugDelay[0]) {
        try {
            sm_debugger_delay = std::stof(debugDelay);
            fmt::print("[SM_DEBUGGER] Using delay: %f\n", sm_debugger_delay);
        } catch (std::invalid_argument& e) {
            fmt::print(
                "Can't convert DebuggerWaitTime from core.cfg. Invalid argument: "
                "[%s]\n",
                debugDelay);
            fmt::print("[SM_DEBUGGER] Using default delay: %f\n", sm_debugger_delay);
        } catch (std::out_of_range& e) {
            fmt::print(
                "Can't convert DebuggerWaitTime from core.cfg. float is out "
                "of range! [%s]\n",
                debugDelay);
            fmt::print("[SM_DEBUGGER] Using default delay: %f\n", sm_debugger_delay);
        } catch (...) {
            fmt::print(
                "Can't convert DebuggerWaitTime from core.cfg. unknown problem! "
                "[%s]\n",
                debugDelay);
            fmt::print("[SM_DEBUGGER] Using default delay: %f\n", sm_debugger_delay);
        }
    }

    // Print the values we're using (automatically, without needing to type)
    fmt::print("[SM_DEBUGGER] Using port: {} and delay: {}\n", sm_debugger_port, sm_debugger_delay);

    // Try x64 version first
    auto module = GetModuleHandle(modulename_x64.c_str());
    if (!module) {
        // Fall back to x86 if x64 not found
        module = GetModuleHandle(modulename_x86.c_str());
    }

    if (module) {
        factoryFn = GetSourcePawnFactoryFn(
            GetProcAddress((HMODULE)module, "GetSourcePawnFactory"));
    }
    if (factoryFn) {
        factory = factoryFn(LOWEST_SOURCEPAWN_API_VERSION);
    }
    if (factory) {
        current_env = factory->CurrentEnvironment();
    }
    if (current_env) {
        if (!Inited) {
            std::thread(debugThread).detach();
            Inited = true;
        }
        current_env->EnableDebugBreak();
        DebugListener.original = current_env->APIv1()->SetDebugListener(&DebugListener);
        current_env->APIv1()->SetDebugBreakHandler(DebugHandler);
        std::this_thread::sleep_for(
            std::chrono::duration<float>(SM_Debugger_timeout()));
    }
    return true;
}

void Extension::SDK_OnUnload()
{
    ISourcePawnFactory* factory = nullptr;
    GetSourcePawnFactoryFn factoryFn = nullptr;
    ISourcePawnEnvironment* current_env = nullptr;

    // Try both x64 and x86 versions for unloading
    std::string modulename_x64 = "sourcepawn.jit.x64." PLATFORM_LIB_EXT;
    std::string modulename_x86 = "sourcepawn.jit.x86." PLATFORM_LIB_EXT;

    auto module = GetModuleHandle(modulename_x64.c_str());
    if (!module) {
        module = GetModuleHandle(modulename_x86.c_str());
    }

    if (module) {
        factoryFn = GetSourcePawnFactoryFn(
            GetProcAddress((HMODULE)module, "GetSourcePawnFactory"));
    }
    if (factoryFn) {
        factory = factoryFn(SOURCEPAWN_API_VERSION);
    }
    if (factory) {
        current_env = factory->CurrentEnvironment();
    }
    if (current_env) {
        current_env->APIv1()->SetDebugListener(DebugListener.original);
    }
}

void Extension::SDK_OnAllLoaded() { }

void Extension::SDK_OnPauseChange(bool paused) { }

void Extension::SDK_OnDependenciesDropped() { }
/*
bool Extension::RegisterConCommandBase(ConCommandBase* pVar) {
        return META_REGCVAR(pVar);
}
*/