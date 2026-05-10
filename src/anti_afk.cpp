#define _CRT_SECURE_NO_WARNINGS
#include "MinHook.h"
#include <fstream>
#include <stdlib.h>
#include <string>
#include <windows.h>

extern "C" __declspec(dllexport) void DummyExport() {}

// ============================================================================
// Configuration
// ============================================================================
int g_intervalMinutes = 4;
bool g_enabled = true;
bool g_debugLog = false;

// ============================================================================
// Compatibility & Version Definitions
// ============================================================================
// If a game update changes method signatures, update these constants
#define EXPECTED_GETINSTANCE_PARAMS 0
#define EXPECTED_GETSENDER_PARAMS 0
#define EXPECTED_SEND_PARAMS 1
#define EXPECTED_CTOR_PARAMS 0

// Shutdown event - signaled on DLL_PROCESS_DETACH to wake sleeping threads
HANDLE g_shutdownEvent = NULL;

void DebugLog(const char *fmt, ...) {
  if (!g_debugLog)
    return;
  char buf[1024];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);

  char path[MAX_PATH];
  GetModuleFileNameA(NULL, path, MAX_PATH);
  std::string dir(path);
  size_t pos = dir.find_last_of("\\/");
  if (pos != std::string::npos)
    dir = dir.substr(0, pos + 1);
  std::string logPath = dir + "plugin\\anti_afk_log.txt";

  FILE *f = fopen(logPath.c_str(), "a");
  if (f) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(f, "[%02d:%02d:%02d] %s\n", st.wHour, st.wMinute, st.wSecond, buf);
    fclose(f);
  }
}

void LoadConfig() {
  char path[MAX_PATH];
  GetModuleFileNameA(NULL, path, MAX_PATH);

  std::string dir(path);
  size_t pos = dir.find_last_of("\\/");
  if (pos != std::string::npos)
    dir = dir.substr(0, pos + 1);

  std::string configPath = dir + "plugin\\anti_afk_config.txt";
  std::ifstream config(configPath);
  if (config.is_open()) {
    std::string line;
    while (std::getline(config, line)) {
      if (line.empty() || line[0] == '#' || line[0] == ';')
        continue;
      size_t eq = line.find('=');
      if (eq == std::string::npos)
        continue;
      std::string key = line.substr(0, eq);
      std::string val = line.substr(eq + 1);
      while (!key.empty() && key.back() == ' ')
        key.pop_back();
      while (!val.empty() && val.front() == ' ')
        val = val.substr(1);
      if (key == "interval_minutes") {
        int v = atoi(val.c_str());
        if (v >= 1 && v <= 30)
          g_intervalMinutes = v;
      } else if (key == "enabled") {
        g_enabled = (val == "1" || val == "true");
      } else if (key == "debug_log") {
        g_debugLog = (val == "1" || val == "true");
      }
    }
    config.close();
  }
}

// ============================================================================
// Il2Cpp API Definitions
// ============================================================================
typedef void *(*Il2CppDomainGet)();
typedef void *(*Il2CppThreadAttach)(void *domain);
typedef void *(*Il2CppGetImage)(void *assembly);
typedef void *(*Il2CppDomainGetAssemblies)(void *domain, size_t *size);
typedef void *(*Il2CppClassFromName)(void *image, const char *namespaze,
                                     const char *name);
typedef void *(*Il2CppClassGetMethodFromName)(void *klass, const char *name,
                                              int argsCount);
typedef void *(*Il2CppRuntimeInvoke)(void *method, void *obj, void **params,
                                     void **exc);
typedef void *(*Il2CppObjectNew)(void *klass);
typedef void *(*Il2CppClassGetPropertyFromName)(void *klass, const char *name);
typedef void *(*Il2CppPropertyGetGetMethod)(void *prop);
typedef void *(*Il2CppFieldSetValue)(void *obj, void *field, void *value);
typedef void *(*Il2CppClassGetFieldFromName)(void *klass, const char *name);
typedef const char *(*Il2CppClassGetName)(void *klass);
typedef const char *(*Il2CppClassGetNamespace)(void *klass);

Il2CppDomainGet il2cpp_domain_get;
Il2CppThreadAttach il2cpp_thread_attach;
Il2CppDomainGetAssemblies il2cpp_domain_get_assemblies;
Il2CppClassFromName il2cpp_class_from_name;
Il2CppClassGetMethodFromName il2cpp_class_get_method_from_name;
Il2CppRuntimeInvoke il2cpp_runtime_invoke;
Il2CppGetImage il2cpp_assembly_get_image;
Il2CppObjectNew il2cpp_object_new;
Il2CppClassGetPropertyFromName il2cpp_class_get_property_from_name;
Il2CppPropertyGetGetMethod il2cpp_property_get_get_method;
Il2CppFieldSetValue il2cpp_field_set_value;
Il2CppClassGetFieldFromName il2cpp_class_get_field_from_name;
Il2CppClassGetName il2cpp_class_get_name;
Il2CppClassGetNamespace il2cpp_class_get_namespace;

HMODULE hGameAssembly = NULL;

// ============================================================================
// Il2Cpp Resolution
// ============================================================================
bool ResolveIl2Cpp() {
  hGameAssembly = GetModuleHandleW(L"GameAssembly.dll");
  if (!hGameAssembly)
    return false;

#define RESOLVE(name)                                                          \
  name = (decltype(name))GetProcAddress(hGameAssembly, #name);                 \
  if (!name) {                                                                 \
    DebugLog("Failed to resolve: %s", #name);                                  \
    return false;                                                              \
  }

  RESOLVE(il2cpp_domain_get);
  RESOLVE(il2cpp_thread_attach);
  RESOLVE(il2cpp_domain_get_assemblies);
  RESOLVE(il2cpp_class_from_name);
  RESOLVE(il2cpp_class_get_method_from_name);
  RESOLVE(il2cpp_runtime_invoke);
  RESOLVE(il2cpp_assembly_get_image);
  RESOLVE(il2cpp_object_new);

  il2cpp_class_get_property_from_name =
      (Il2CppClassGetPropertyFromName)GetProcAddress(
          hGameAssembly, "il2cpp_class_get_property_from_name");
  il2cpp_property_get_get_method = (Il2CppPropertyGetGetMethod)GetProcAddress(
      hGameAssembly, "il2cpp_property_get_get_method");
  il2cpp_field_set_value = (Il2CppFieldSetValue)GetProcAddress(
      hGameAssembly, "il2cpp_field_set_value");
  il2cpp_class_get_field_from_name =
      (Il2CppClassGetFieldFromName)GetProcAddress(
          hGameAssembly, "il2cpp_class_get_field_from_name");
  il2cpp_class_get_name = (Il2CppClassGetName)GetProcAddress(
      hGameAssembly, "il2cpp_class_get_name");
  il2cpp_class_get_namespace = (Il2CppClassGetNamespace)GetProcAddress(
      hGameAssembly, "il2cpp_class_get_namespace");

#undef RESOLVE
  return true;
}

// ============================================================================
// Assembly helpers
// ============================================================================
void *FindClassInAssemblies(void **assemblies, size_t count, const char *ns,
                            const char *name) {
  for (size_t i = 0; i < count; i++) {
    void *image = il2cpp_assembly_get_image(assemblies[i]);
    void *klass = il2cpp_class_from_name(image, ns, name);
    if (klass)
      return klass;
  }
  return nullptr;
}

// ============================================================================
// Anti-AFK core logic
// ============================================================================
static void *g_netBusClass = nullptr;
static void *g_getInstanceMethod = nullptr;
static void *g_getSenderMethod = nullptr;
static void *g_sendMethod = nullptr;

// Proto message classes (priority: FRIEND_SYNC > SCENE_REST > PING)
static void *g_friendSyncClass = nullptr;
static void *g_sceneRestClass = nullptr;
static void *g_pingClass = nullptr;
static void *g_pingField = nullptr;
static const char *g_activeMsgName = nullptr;

bool ResolveSendMethod(void **assemblies, size_t count) {
  const char *nsVariants[] = {"Beyond.Network", "Beyond", ""};
  for (auto ns : nsVariants) {
    g_netBusClass = FindClassInAssemblies(assemblies, count, ns, "NetBus");
    if (g_netBusClass) {
      DebugLog("Found NetBus in namespace: '%s'", ns);
      break;
    }
  }
  if (!g_netBusClass) {
    DebugLog("ERROR: Could not find NetBus class");
    return false;
  }

  if (il2cpp_class_get_property_from_name && il2cpp_property_get_get_method) {
    void *prop = il2cpp_class_get_property_from_name(g_netBusClass, "instance");
    if (prop) {
      g_getInstanceMethod = il2cpp_property_get_get_method(prop);
      DebugLog("Found NetBus.instance via property");
    }
  }
  if (!g_getInstanceMethod)
    g_getInstanceMethod =
        il2cpp_class_get_method_from_name(g_netBusClass, "get_instance", EXPECTED_GETINSTANCE_PARAMS);
  if (!g_getInstanceMethod) {
    DebugLog("ERROR: Could not find NetBus.instance getter");
    return false;
  }

  if (il2cpp_class_get_property_from_name && il2cpp_property_get_get_method) {
    void *prop =
        il2cpp_class_get_property_from_name(g_netBusClass, "defaultSender");
    if (prop) {
      g_getSenderMethod = il2cpp_property_get_get_method(prop);
      DebugLog("Found NetBus.defaultSender via property");
    }
  }
  if (!g_getSenderMethod)
    g_getSenderMethod = il2cpp_class_get_method_from_name(
        g_netBusClass, "get_defaultSender", EXPECTED_GETSENDER_PARAMS);
  if (!g_getSenderMethod) {
    DebugLog("ERROR: Could not find NetBus.defaultSender getter");
    return false;
  }

  return true;
}

bool ResolveSenderSendMethod(void *sender) {
  if (g_sendMethod)
    return true;

  typedef void *(*Il2CppObjectGetClass)(void *obj);
  auto il2cpp_object_get_class = (Il2CppObjectGetClass)GetProcAddress(
      hGameAssembly, "il2cpp_object_get_class");

  if (il2cpp_object_get_class && sender) {
    void *senderClass = il2cpp_object_get_class(sender);
    if (senderClass) {
      if (il2cpp_class_get_name)
        DebugLog("Sender class: %s", il2cpp_class_get_name(senderClass));
      g_sendMethod = il2cpp_class_get_method_from_name(senderClass, "Send", EXPECTED_SEND_PARAMS);
      if (g_sendMethod) {
        DebugLog("Found sender.Send(msg) method");
        return true;
      }
    }
  }
  DebugLog("ERROR: Could not find Send method on sender");
  return false;
}

bool ResolveProtoMessages(void **assemblies, size_t count) {
  // Priority 1: CS_FRIEND_LIST_SIMPLE_SYNC (natural, no context needed)
  g_friendSyncClass = FindClassInAssemblies(assemblies, count, "Proto",
                                            "CS_FRIEND_LIST_SIMPLE_SYNC");
  if (g_friendSyncClass)
    DebugLog("Found proto: CS_FRIEND_LIST_SIMPLE_SYNC (primary)");

  // Priority 2: CS_SCENE_REST (empty message)
  g_sceneRestClass =
      FindClassInAssemblies(assemblies, count, "Proto", "CS_SCENE_REST");
  if (g_sceneRestClass)
    DebugLog("Found proto: CS_SCENE_REST (secondary)");

  // Priority 3: CS_PING (may not count as activity)
  g_pingClass = FindClassInAssemblies(assemblies, count, "Proto", "CS_PING");
  if (g_pingClass) {
    DebugLog("Found proto: CS_PING (fallback)");
    if (il2cpp_class_get_field_from_name)
      g_pingField = il2cpp_class_get_field_from_name(g_pingClass, "clientTs_");
  }

  if (!g_friendSyncClass && !g_sceneRestClass && !g_pingClass) {
    DebugLog("ERROR: Could not find any suitable proto message class");
    return false;
  }
  return true;
}

bool SendAntiAFKPacket() {
  void *netBusInstance =
      il2cpp_runtime_invoke(g_getInstanceMethod, nullptr, nullptr, nullptr);
  if (!netBusInstance) {
    DebugLog("NetBus.instance returned null");
    return false;
  }

  void *sender = il2cpp_runtime_invoke(g_getSenderMethod, netBusInstance,
                                       nullptr, nullptr);
  if (!sender) {
    DebugLog("NetBus.defaultSender returned null (not connected?)");
    return false;
  }

  if (!ResolveSenderSendMethod(sender))
    return false;

  void *msg = nullptr;

  if (g_friendSyncClass) {
    msg = il2cpp_object_new(g_friendSyncClass);
    if (msg) {
      void *ctor =
          il2cpp_class_get_method_from_name(g_friendSyncClass, ".ctor", EXPECTED_CTOR_PARAMS);
      if (ctor)
        il2cpp_runtime_invoke(ctor, msg, nullptr, nullptr);
      g_activeMsgName = "CS_FRIEND_LIST_SIMPLE_SYNC";
    }
  } else if (g_sceneRestClass) {
    msg = il2cpp_object_new(g_sceneRestClass);
    if (msg) {
      void *ctor =
          il2cpp_class_get_method_from_name(g_sceneRestClass, ".ctor", EXPECTED_CTOR_PARAMS);
      if (ctor)
        il2cpp_runtime_invoke(ctor, msg, nullptr, nullptr);
      g_activeMsgName = "CS_SCENE_REST";
    }
  } else if (g_pingClass) {
    msg = il2cpp_object_new(g_pingClass);
    if (msg) {
      void *ctor = il2cpp_class_get_method_from_name(g_pingClass, ".ctor", EXPECTED_CTOR_PARAMS);
      if (ctor)
        il2cpp_runtime_invoke(ctor, msg, nullptr, nullptr);
      if (g_pingField && il2cpp_field_set_value) {
        ULONGLONG ts = GetTickCount64();
        il2cpp_field_set_value(msg, g_pingField, &ts);
      }
      g_activeMsgName = "CS_PING";
    }
  }

  if (!msg) {
    DebugLog("Failed to create proto message");
    return false;
  }

  void *sendParams[] = {msg};
  void *exc = nullptr;
  
  __try {
    il2cpp_runtime_invoke(g_sendMethod, sender, sendParams, &exc);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    DebugLog("ERROR: SEH Exception caught during g_sendMethod invocation! Ignored to prevent crash.");
    return false;
  }

  if (exc) {
    DebugLog("Send threw an exception");
    return false;
  }

  DebugLog("Sent %s successfully", g_activeMsgName);
  return true;
}

// ============================================================================
// Stealth: Hide DLL from file system scans
// ============================================================================
typedef HANDLE(WINAPI *CREATE_FILE_W)(LPCWSTR, DWORD, DWORD,
                                      LPSECURITY_ATTRIBUTES, DWORD, DWORD,
                                      HANDLE);
static CREATE_FILE_W p_CreateFileW = NULL;
typedef DWORD(WINAPI *GET_FILE_ATTRIBUTES_W)(LPCWSTR);
static GET_FILE_ATTRIBUTES_W p_GetFileAttributesW = NULL;

bool ShouldHide(LPCWSTR name) {
  if (!name)
    return false;
  std::wstring n(name);
  for (auto &c : n)
    c = towlower(c);
  return (n.find(L"anti_afk.dll") != std::wstring::npos);
}

HANDLE WINAPI h_CreateFileW(LPCWSTR lpFileName, DWORD dwDesiredAccess,
                            DWORD dwShareMode,
                            LPSECURITY_ATTRIBUTES lpSecurityAttributes,
                            DWORD dwCreationDisposition,
                            DWORD dwFlagsAndAttributes, HANDLE hTemplateFile) {
  if (ShouldHide(lpFileName)) {
    SetLastError(ERROR_FILE_NOT_FOUND);
    return INVALID_HANDLE_VALUE;
  }
  return p_CreateFileW(lpFileName, dwDesiredAccess, dwShareMode,
                       lpSecurityAttributes, dwCreationDisposition,
                       dwFlagsAndAttributes, hTemplateFile);
}

DWORD WINAPI h_GetFileAttributesW(LPCWSTR lpFileName) {
  if (ShouldHide(lpFileName)) {
    SetLastError(ERROR_FILE_NOT_FOUND);
    return INVALID_FILE_ATTRIBUTES;
  }
  return p_GetFileAttributesW(lpFileName);
}

// ============================================================================
// Main anti-AFK thread
// ============================================================================
static void *g_domain = nullptr;

DWORD WINAPI AntiAFKThread(LPVOID lpParam) {
  DebugLog("Anti-AFK thread started, interval=%d minutes", g_intervalMinutes);

  // Wait for game init (interruptible via shutdown event)
  if (WaitForSingleObject(g_shutdownEvent, 30000) == WAIT_OBJECT_0) {
    DebugLog("Shutdown during init wait");
    return 0;
  }

  if (!ResolveIl2Cpp()) {
    DebugLog("Failed to resolve Il2Cpp APIs");
    return 1;
  }
  DebugLog("Il2Cpp APIs resolved");

  // Resolve il2cpp_thread_detach
  typedef void (*Il2CppThreadDetach)(void *thread);
  auto il2cpp_thread_detach =
      (Il2CppThreadDetach)GetProcAddress(hGameAssembly, "il2cpp_thread_detach");

  g_domain = il2cpp_domain_get();
  if (!g_domain) {
    DebugLog("Failed to get Il2Cpp domain");
    return 1;
  }

  // Attach to resolve classes, then detach
  void *thread = (void *)il2cpp_thread_attach(g_domain);

  size_t asmCount = 0;
  void **assemblies =
      (void **)il2cpp_domain_get_assemblies(g_domain, &asmCount);
  if (!assemblies || asmCount == 0) {
    DebugLog("Failed to get assemblies");
    if (il2cpp_thread_detach && thread)
      il2cpp_thread_detach(thread);
    return 1;
  }
  DebugLog("Found %zu assemblies", asmCount);

  if (!ResolveSendMethod(assemblies, asmCount)) {
    DebugLog("Failed to resolve send method chain");
    if (il2cpp_thread_detach && thread)
      il2cpp_thread_detach(thread);
    return 1;
  }

  if (!ResolveProtoMessages(assemblies, asmCount)) {
    DebugLog("Failed to resolve proto messages");
    if (il2cpp_thread_detach && thread)
      il2cpp_thread_detach(thread);
    return 1;
  }

  // Detach after init - sleep in detached state so Il2Cpp won't wait for us
  if (il2cpp_thread_detach && thread)
    il2cpp_thread_detach(thread);

  DebugLog("All resolved, entering main loop (using %s)",
           g_friendSyncClass  ? "CS_FRIEND_LIST_SIMPLE_SYNC"
           : g_sceneRestClass ? "CS_SCENE_REST"
                              : "CS_PING");

  // Main loop with randomized interval
  srand((unsigned int)GetTickCount());
  DWORD baseMs = g_intervalMinutes * 60 * 1000;
  while (true) {
    DWORD jitter = baseMs * 30 / 100;
    DWORD intervalMs = baseMs - jitter + (rand() % (jitter * 2 + 1));
    DebugLog("Next packet in %lu seconds", intervalMs / 1000);
    DWORD waitResult = WaitForSingleObject(g_shutdownEvent, intervalMs);
    if (waitResult == WAIT_OBJECT_0)
      break;

    // Attach briefly just for the send, then detach
    void *t = (void *)il2cpp_thread_attach(g_domain);
    __try {
      SendAntiAFKPacket();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      DebugLog("ERROR: SEH Exception caught inside SendAntiAFKPacket routine! Recovering safely.");
    }
    if (il2cpp_thread_detach && t)
      il2cpp_thread_detach(t);
  }

  DebugLog("Anti-AFK thread exiting cleanly");
  return 0;
}

// ============================================================================
// DLL Entry Point
// ============================================================================
static HANDLE g_thread = NULL;

void Setup() {
  HANDLE hMutex = CreateMutexA(
      NULL, TRUE, "Local\\ArknightsEndfield_AntiAFK_InstanceGuard");
  if (GetLastError() == ERROR_ALREADY_EXISTS) {
    if (hMutex)
      CloseHandle(hMutex);
    return;
  }

  LoadConfig();
  if (!g_enabled)
    return;

  DebugLog("=== Anti-AFK DLL Loaded ===");
  DebugLog("Interval: %d minutes", g_intervalMinutes);

  g_shutdownEvent = CreateEventW(NULL, TRUE, FALSE, NULL);

  // Install stealth hooks to hide anti_afk.dll from file scans
  MH_Initialize();
  MH_CreateHookApi(L"kernelbase.dll", "CreateFileW", (LPVOID)h_CreateFileW,
                   (LPVOID *)&p_CreateFileW);
  MH_CreateHookApi(L"kernelbase.dll", "GetFileAttributesW",
                   (LPVOID)h_GetFileAttributesW,
                   (LPVOID *)&p_GetFileAttributesW);
  MH_EnableHook(MH_ALL_HOOKS);
  DebugLog("Stealth hooks installed");

  g_thread = CreateThread(NULL, 0, AntiAFKThread, NULL, 0, NULL);
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID reserved) {
  if (reason == DLL_PROCESS_ATTACH) {
    DisableThreadLibraryCalls(hModule);
    CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)Setup, NULL, 0, NULL);
  } else if (reason == DLL_PROCESS_DETACH) {
    if (reserved != NULL)
      return TRUE; // process terminating, skip cleanup

    if (g_shutdownEvent)
      SetEvent(g_shutdownEvent);
    if (g_thread) {
      WaitForSingleObject(g_thread, 2000);
      CloseHandle(g_thread);
    }
    if (g_shutdownEvent)
      CloseHandle(g_shutdownEvent);
  }
  return TRUE;
}
