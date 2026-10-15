#define _CRT_SECURE_NO_WARNINGS
#define NOMINMAX
#define APPLEPIE_PLUGIN_IMPL
#include "applepie_mgr.h" // Legacy metadata/config UI only; lifecycle uses APH.
#include <applepie_client.h>
#include <atomic>
#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <cwchar>

static applepie::Client g_client;
static APH_RuntimeApi g_runtime{};
static std::atomic<bool> g_stopping{false}, g_ready{false};
static std::atomic<uint64_t> g_epoch{0};
// One atomic snapshot: interval in bits 0..7, enabled/debug in bits 8/9.
static std::atomic<uint32_t> g_config{4u | 0x100u};
static HANDLE g_cancel = nullptr, g_worker = nullptr;
static SRWLOCK g_workerLock = SRWLOCK_INIT;
static wchar_t g_configPath[MAX_PATH]{}, g_logPath[MAX_PATH]{};

static bool RuntimeOpen() {
  if (g_stopping.load(std::memory_order_acquire)) return false;
  if (g_runtime.probe_open) return g_runtime.probe_open(g_client.owner()) == APH_OK;
  APH_RuntimeSnapshot s{};
  return g_runtime.snapshot && g_runtime.snapshot(sizeof(s), &s) == APH_OK && s.state < APH_RUNTIME_STOPPING;
}
static bool EnableOpen(uint64_t epoch) {
  APH_OwnerStatus s{};
  return WaitForSingleObject(g_cancel, 0) == WAIT_TIMEOUT && RuntimeOpen() &&
    g_client.Status(s) == APH_OK && s.state == APH_OWNER_ENABLING && s.activation_epoch == epoch;
}
static bool HideEnabled() {
  return g_ready.load(std::memory_order_acquire) && (g_config.load() & 0x100u);
}
static void BeginRuntimeStop() {
  g_stopping.store(true, std::memory_order_release);
  g_ready.store(false, std::memory_order_release);
  if (g_cancel) SetEvent(g_cancel);
}

static bool InitPaths() {
  wchar_t path[MAX_PATH]{};
  DWORD size = GetModuleFileNameW(nullptr, path, _countof(path));
  if (!size || size >= _countof(path)) return false;
  wchar_t* slash = wcsrchr(path, L'\\');
  if (!slash) return false;
  slash[1] = 0;
  if (wcslen(path) + wcslen(L"plugin\\anti_afk_config.txt") >= _countof(g_configPath)) return false;
  return swprintf_s(g_configPath, L"%splugin\\anti_afk_config.txt", path) > 0 &&
         swprintf_s(g_logPath, L"%splugin\\anti_afk_log.txt", path) > 0;
}
static void WriteLog(const char* fmt, va_list args) {
  const DWORD savedError = GetLastError();
  char buffer[1024];
  vsnprintf(buffer, sizeof(buffer), fmt, args);
  FILE* f = _wfopen(g_logPath, L"a");
  if (f) {
    SYSTEMTIME st{}; GetLocalTime(&st);
    fprintf(f, "[%02d:%02d:%02d] %s\n", st.wHour, st.wMinute, st.wSecond, buffer);
    fclose(f);
  }
  SetLastError(savedError);
}
static void DiagnosticLog(const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  WriteLog(fmt, args);
  va_end(args);
}
static void DebugLog(const char* fmt, ...) {
  if (!(g_config.load() & 0x200u)) return;
  va_list args;
  va_start(args, fmt);
  WriteLog(fmt, args);
  va_end(args);
}
static void LoadConfig() {
  uint32_t config = 4u | 0x100u;
  FILE* f = _wfopen(g_configPath, L"r");
  if (f) {
    char line[512], key[128], value[128];
    while (fgets(line, sizeof(line), f)) {
      if (line[0] == '#' || line[0] == ';') continue;
      if (sscanf(line, " %127[^=]= %127s", key, value) != 2) continue;
      size_t len = strlen(key);
      while (len && (key[len - 1] == ' ' || key[len - 1] == '\t')) key[--len] = 0;
      if (!strcmp(key, "interval_minutes")) {
        char* end = nullptr;
        long n = strtol(value, &end, 10);
        if (end != value && !*end && n >= 1 && n <= 30) config = (config & ~255u) | (uint32_t)n;
      } else if (!strcmp(key, "enabled") || !strcmp(key, "debug_log")) {
        uint32_t bit = !strcmp(key, "enabled") ? 0x100u : 0x200u;
        if (!strcmp(value, "1") || !strcmp(value, "true")) config |= bit;
        else config &= ~bit;
      }
    }
    fclose(f);
  }
  g_config.store(config, std::memory_order_release);
}

#include "managed_runtime.h"
#include "managed_hooks.h"

// Read-only diagnostic probe. Never interpret or follow a rejected branch.
static bool ReadHookPrefix(APH_NativeProc target, unsigned char (&bytes)[2]) {
  __try { memcpy(bytes, (const void*)target, sizeof(bytes)); return true; }
  __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static void LogHookInstall(const char* label, APH_NativeProc target, APH_Result result, uint64_t claim) {
  const DWORD savedError = GetLastError();
  HMODULE module = nullptr;
  if (target) GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                (LPCWSTR)target, &module);
  DiagnosticLog("Hook %s target=%p module=%p rva=0x%llx claim=%llu result=%d", label, (void*)target,
                (void*)module, module ? (unsigned long long)((uintptr_t)target - (uintptr_t)module) : 0,
                (unsigned long long)claim, result);
  if (result == APH_E_FOREIGN_PATCH && !claim) {
    unsigned char bytes[2]{};
    if (ReadHookPrefix(target, bytes))
      DiagnosticLog("Rejected entry %s prefix=%02X %02X (no branch traversal)", label, bytes[0], bytes[1]);
  }
  SetLastError(savedError);
}

// Callback-owned fields: reset only after the previous epoch has drained.
static uint64_t g_nextDue = 0;
static uint32_t g_timerConfig = 0, g_random = 1;
static DWORD g_gameThread = 0;
static HWND g_gameWindow = nullptr;
static std::atomic_flag g_tickBusy = ATOMIC_FLAG_INIT;
static uint32_t g_unreleasedRoots[3]{};
static std::atomic<bool> g_cleanupUncertain{false};

static BOOL CALLBACK FindUnityWindow(HWND window, LPARAM result) {
  DWORD process = 0;
  GetWindowThreadProcessId(window, &process);
  wchar_t name[64]{};
  if (process == GetCurrentProcessId() && GetClassNameW(window, name, _countof(name)) &&
      wcscmp(name, L"UnityWndClass") == 0) {
    *(HWND*)result = window;
    return FALSE;
  }
  return TRUE;
}
static bool GameThreadVerified() {
  const DWORD current = GetCurrentThreadId();
  if (g_gameWindow && IsWindow(g_gameWindow) && GetWindowThreadProcessId(g_gameWindow, nullptr) == current)
    return g_gameThread == current && g_il.thread_current() != nullptr;
  HWND window = nullptr;
  EnumThreadWindows(current, FindUnityWindow, (LPARAM)&window);
  if (!window || !g_il.thread_current()) return false;
  g_gameWindow = window;
  g_gameThread = current;
  return true;
}
static DWORD NextInterval(uint32_t config) {
  g_random ^= g_random << 13; g_random ^= g_random >> 17; g_random ^= g_random << 5;
  const DWORD base = (config & 255u) * 60000u;
  const DWORD jitter = base * 30u / 100u;
  return base - jitter + g_random % (jitter * 2u + 1u);
}
static void ReleaseRoots(uint32_t (&roots)[3]) {
  __try {
    for (unsigned i = 3; i > 0; --i) {
      if (!roots[i - 1]) continue;
      if (!RuntimeOpen()) break;
      g_il.gchandle_free(roots[i - 1]);
      roots[i - 1] = 0;
    }
  } __finally {
    // A shutdown or native cleanup fault forbids further VM calls. Keep the
    // handles as evidence until process exit; never claim a clean hot disable.
    bool retained = false;
    for (unsigned i = 0; i < 3; ++i) if (roots[i]) {
      g_unreleasedRoots[i] = roots[i]; retained = true;
    }
    if (retained) g_cleanupUncertain.store(true, std::memory_order_release);
  }
}

static bool SendAntiAFKPacket() {
  if (!HideEnabled() || !RuntimeOpen()) return false;
  // Roots protect borrowed results across subsequent managed allocations.
  // No object, sender or session survives a normal callback. Failed root
  // cleanup is retained separately until process exit.
  uint32_t roots[3]{};
  bool sent = false;
  __try {
    void* bus = ReadNetBus();
    if (!bus) return false;
    roots[0] = g_il.gchandle_new(bus, true);
    if (!roots[0] || g_il.object_get_class(bus) != g_net.netBus) return false;
    bool valid = false;
    int32_t session = 0;
    if (!InvokeValue(g_net.validateSession, bus, valid) || !valid ||
        !InvokeValue(g_net.sessionId, bus, session)) return false;
    void* sender = nullptr;
    if (!Invoke(g_net.sender, bus, nullptr, &sender) || !sender) return false;
    roots[1] = g_il.gchandle_new(sender, true);
    if (!roots[1]) return false;
    void* senderClass = g_il.object_get_class(sender);
    void* send = FindMethod(senderClass, "Send", false, "Beyond.Network.NetBus.ResultHandler", "Google.Protobuf.IMessage");
    if (!send || !ReferenceType(g_il.method_get_return_type(send)) ||
        !ReferenceType(g_il.method_get_param(send, 0))) {
      DebugLog("Sender.Send has an unsupported signature"); return false;
    }
    const ProtoMessage* proto = nullptr;
    for (const auto& candidate : g_net.messages) if (candidate.klass) { proto = &candidate; break; }
    if (!proto || !g_il.class_is_assignable_from(g_il.class_from_type(g_il.method_get_param(send, 0)), proto->klass))
      return false;
    if (!HideEnabled() || !RuntimeOpen()) return false;
    void* message = g_il.object_new(proto->klass);
    if (!message) return false;
    roots[2] = g_il.gchandle_new(message, true);
    if (!roots[2] || !Invoke(proto->ctor, message, nullptr, nullptr)) return false;
    if (proto->timestamp) {
      if (!RuntimeOpen()) return false;
      uint64_t timestamp = GetTickCount64();
      // Newly created, not yet shared protobuf; no Animation/Camera write.
      g_il.field_set_value(message, proto->timestamp, &timestamp);
    }
    int32_t currentSession = 0;
    void* currentBus = ReadNetBus();
    void* currentSender = nullptr;
    if (currentBus != bus ||
        !Invoke(g_net.sender, bus, nullptr, &currentSender) || currentSender != sender ||
        !InvokeValue(g_net.sessionId, bus, currentSession) || currentSession != session ||
        !InvokeValue(g_net.validateSession, bus, valid) || !valid || !HideEnabled()) return false;
    void* args[] = {message};
    if (!Invoke(send, sender, args, nullptr)) {
      // The send may already have been submitted. Never replay an uncertain
      // external operation; pause until an explicit new enable epoch.
      DebugLog("Send outcome uncertain; disabling without replay or rollback");
      g_ready.store(false);
      g_client.Disable();
      return false;
    }
    sent = true;
    DebugLog("Sent %s successfully (session=%d)", proto->name, session);
  } __finally {
    ReleaseRoots(roots);
  }
  return sent;
}

static APH_Result APH_CALL OnUpdate(APH_Owner, const APH_SharedEvent* event, void*) {
  if (!event || event->phase != APH_AFTER || event->payload_bytes != sizeof(UpdatePayload) ||
      event->activation_epoch != g_epoch.load() || !g_ready.load(std::memory_order_acquire)) return APH_OK;
  const auto& p = *(const UpdatePayload*)event->payload;
  if (!p.self || g_stopping.load()) return APH_OK;
  if (g_tickBusy.test_and_set(std::memory_order_acquire)) return APH_OK;
  __try {
  const uint32_t config = g_config.load(std::memory_order_acquire);
  const uint64_t now = GetTickCount64();
  if (config != g_timerConfig) {
    g_timerConfig = config;
    g_nextDue = now + NextInterval(config);
  }
  if (!(config & 0x100u) || now < g_nextDue) return APH_OK;
  // Only due work uses runtime APIs/reflection; no per-frame service lookup,
  // install polling, metadata scans, allocations or file I/O.
  if (!RuntimeOpen() || !GameThreadVerified()) return APH_OK;
  g_nextDue = now + NextInterval(config);
  int32_t state = 0;
  if (!InvokeValue(g_app.currentState, p.self, state) || state != g_app.mainState) return APH_OK;
  if (!g_net.netBus) {
    void* domain = g_il.domain_get();
    size_t count = 0;
    void** assemblies = domain ? g_il.domain_get_assemblies(domain, &count) : nullptr;
    if (!assemblies || !count || !ResolveNetworkMetadata(assemblies, count)) {
      DiagnosticLog("Network metadata unavailable after GameState.Main; disabling this epoch");
      g_ready.store(false, std::memory_order_release);
      g_client.Disable();
      return APH_OK;
    }
    DiagnosticLog("GameState.Main confirmed; network metadata ready on game thread (singleton field)");
  }
  SendAntiAFKPacket();
  return APH_OK;
  } __finally { g_tickBusy.clear(std::memory_order_release); }
}

static APH_Result Initialize(uint64_t epoch) {
  while (EnableOpen(epoch) && !ResolveIl2Cpp()) {
    if (GetTickCount64() >= g_enableDeadline) return APH_E_UNAVAILABLE;
    WaitForSingleObject(g_cancel, 50);
  }
  if (!EnableOpen(epoch)) return APH_E_REVOKED;
  APH_NativeProc shutdown = (APH_NativeProc)GetProcAddress(g_gameAssembly, "il2cpp_shutdown");
  APH_Result result = APH_OK;
  if (shutdown) {
    result = ShutdownHook::Install(epoch, shutdown, "void() C-export", "il2cpp.runtime.shutdown");
    LogHookInstall("il2cpp_shutdown", shutdown, result, ShutdownHook::hook.handle());
    if (result != APH_OK) {
      // Core deliberately rejects existing branches, including export thunks.
      // Only a rejection BEFORE registration may use the other stop boundary.
      // An existing claim/backend failure still aborts this enable.
      if (result != APH_E_FOREIGN_PATCH || ShutdownHook::hook.handle()) return result;
      DiagnosticLog("Shutdown entry refused before registration; requiring validated ApplicationQuit");
    }
  } else {
    DiagnosticLog("Shutdown export absent; requiring validated ApplicationQuit");
  }
  DiagnosticLog("Waiting for IL2CPP worker readiness (corlib + registered VM thread)");
  while (EnableOpen(epoch) && !WorkerAttachReady()) {
    if (GetTickCount64() >= g_enableDeadline) {
      DiagnosticLog("IL2CPP worker readiness timed out before domain/attach/metadata access");
      return APH_E_UNAVAILABLE;
    }
    WaitForSingleObject(g_cancel, 50);
  }
  if (!EnableOpen(epoch)) return APH_E_REVOKED;
  DiagnosticLog("IL2CPP worker readiness confirmed; discovering lifecycle only, network waits for GameState.Main");
  bool resolved = false;
  while (EnableOpen(epoch) && !resolved) {
    if (GetTickCount64() >= g_enableDeadline) return APH_E_SIGNATURE;
    void* domain = g_il.domain_get();
    void* thread = domain ? g_il.thread_attach(domain) : nullptr;
    if (thread) {
      __try {
        size_t count = 0;
        void** assemblies = g_il.domain_get_assemblies(domain, &count);
        if (assemblies && count && EnableOpen(epoch)) resolved = ResolveLifecycleMetadata(assemblies, count);
      } __finally { if (RuntimeOpen()) g_il.thread_detach(thread); }
    }
    if (!resolved) WaitForSingleObject(g_cancel, 50);
  }
  if (!EnableOpen(epoch)) return APH_E_REVOKED;
  const APH_NativeProc quit = NativeAddress(g_app.quit);
  result = QuitHook::Install(epoch, quit, "void(MethodInfo*) static", "unity.Application.Internal_ApplicationQuit");
  LogHookInstall("Application.Internal_ApplicationQuit", quit, result, QuitHook::hook.handle());
  if (result != APH_OK) return result;
  DiagnosticLog("Stop boundaries: shutdown=%d applicationQuit=1", ShutdownHook::hook.handle() ? 1 : 0);
  const APH_NativeProc update = NativeAddress(g_app.update);
  result = UpdateHook::Install(epoch, update);
  LogHookInstall("GameApp.Update", update, result, UpdateHook::hook.handle());
  if (result != APH_OK) return result;
  HMODULE kernelbase = GetModuleHandleW(L"kernelbase.dll");
  auto createFile = (decltype(&CreateFileW))GetProcAddress(kernelbase, "CreateFileW");
  auto attributes = (decltype(&GetFileAttributesW))GetProcAddress(kernelbase, "GetFileAttributesW");
  result = CreateFileHook::Install(epoch, createFile,
      "HANDLE WINAPI(LPCWSTR,DWORD,DWORD,LPSECURITY_ATTRIBUTES,DWORD,DWORD,HANDLE)", "kernelbase.CreateFileW");
  LogHookInstall("KernelBase.CreateFileW", (APH_NativeProc)createFile, result, CreateFileHook::hook.handle());
  if (result != APH_OK) return result;
  result = AttributesHook::Install(epoch, attributes, "DWORD WINAPI(LPCWSTR)", "kernelbase.GetFileAttributesW");
  LogHookInstall("KernelBase.GetFileAttributesW", (APH_NativeProc)attributes, result, AttributesHook::hook.handle());
  return result;
}
static DWORD WINAPI InitializeWorker(void* argument) {
  const uint64_t epoch = (uint64_t)(uintptr_t)argument;
  const APH_Result result = Initialize(epoch);
  if (result == APH_OK && EnableOpen(epoch)) g_ready.store(true, std::memory_order_release);
  const APH_Result completed = g_client.CompleteEnable(epoch, result);
  if (completed != APH_OK) g_ready.store(false, std::memory_order_release);
  DiagnosticLog("Enable epoch=%llu result=%d completion=%d", (unsigned long long)epoch, result, completed);
  return 0;
}
static void APH_CALL Disabling(APH_Owner, uint64_t, void*) {
  AcquireSRWLockExclusive(&g_workerLock);
  g_ready.store(false, std::memory_order_release);
  SetEvent(g_cancel);
  ReleaseSRWLockExclusive(&g_workerLock);
}
static APH_Result APH_CALL Restore(APH_Owner, const APH_Cleanup* cleanup, void*) {
  if (!cleanup || cleanup->domains) return APH_E_ARGUMENT;
  AcquireSRWLockExclusive(&g_workerLock);
  __try {
    if (g_worker) {
      const DWORD state = WaitForSingleObject(g_worker, 0);
      if (state == WAIT_TIMEOUT) return APH_PENDING;
      if (state != WAIT_OBJECT_0) return APH_E_NEEDS_RESTART;
      CloseHandle(g_worker); g_worker = nullptr;
    }
    if (g_cleanupUncertain.load(std::memory_order_acquire)) return APH_E_NEEDS_RESTART;
    APH_Result result = UpdateHook::listener.Close();
    if (result != APH_OK) return result == APH_E_BUSY ? APH_PENDING : result;
    g_app = {}; g_net = {};
    g_gameWindow = nullptr; g_gameThread = 0;
    g_nextDue = 0; g_timerConfig = 0;
    return APH_OK;
  } __finally { ReleaseSRWLockExclusive(&g_workerLock); }
}

extern "C" __declspec(dllexport) const APH_Manifest APH_PluginManifest = {
  sizeof(APH_Manifest), APH_ABI_MAJOR, APH_ABI_MINOR, APH_ARCH_X64,
  APH_PLUGIN_LIFECYCLE | APH_PLUGIN_LATE_LOAD | APH_PLUGIN_MODULE_OBSERVER,
  APH_CAP_CORE | APH_CAP_HOOKS, 0, "anti_afk", "Anti-AFK"
};
extern "C" __declspec(dllexport) APH_Result APH_CALL APH_Attach(const APH_AttachArgs* args) {
  if (g_cancel) return APH_E_DUPLICATE;
  APH_Result result = g_client.Attach(args);
  if (result != APH_OK) return result;
  result = g_client.Service(g_runtime);
  if (result != APH_OK) return result;
  if (!InitPaths()) return APH_E_UNAVAILABLE;
  g_cancel = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!g_cancel) return APH_E_UNAVAILABLE;
  const APH_LifecycleDesc lifecycle{sizeof(lifecycle), 60000, Disabling, Restore, nullptr};
  result = g_client.RegisterLifecycle(lifecycle);
  if (result != APH_OK) { CloseHandle(g_cancel); g_cancel = nullptr; }
  return result;
}
extern "C" __declspec(dllexport) APH_Result APH_CALL APH_Enable(APH_Owner owner, uint64_t epoch) {
  if (owner != g_client.owner() || !epoch) return APH_E_ARGUMENT;
  AcquireSRWLockExclusive(&g_workerLock);
  __try {
    APH_OwnerStatus status{};
    APH_Result statusResult = g_client.Status(status);
    if (statusResult != APH_OK) return statusResult;
    if (status.state != APH_OWNER_ENABLING || status.activation_epoch != epoch) return APH_E_STALE;
    if (!RuntimeOpen()) return APH_E_REVOKED;
    if (g_worker) {
      if (WaitForSingleObject(g_worker, 0) != WAIT_OBJECT_0) return APH_E_BUSY;
      CloseHandle(g_worker); g_worker = nullptr;
    }
    LoadConfig();
    ResetEvent(g_cancel);
    g_epoch.store(epoch);
    g_ready.store(false);
    g_timerConfig = 0; g_nextDue = 0;
    g_random = (uint32_t)(GetTickCount64() ^ epoch) | 1u;
    g_enableDeadline = GetTickCount64() + 45000;
    g_worker = CreateThread(nullptr, 0, InitializeWorker, (void*)(uintptr_t)epoch, 0, nullptr);
    return g_worker ? APH_PENDING : APH_E_UNAVAILABLE;
  } __finally { ReleaseSRWLockExclusive(&g_workerLock); }
}

static AP_PluginInfo s_pluginInfo = {
  APPLEPIE_PLUGIN_API_VERSION, "anti_afk", "Anti-AFK", "Prevent AFK detection kick",
  "anti_afk_config.txt", true
};
APPLEPIE_PLUGIN_EXPORT AP_PluginInfo* AP_GetPluginInfo() { return &s_pluginInfo; }
APPLEPIE_PLUGIN_EXPORT bool AP_ReloadConfig() { LoadConfig(); return true; }
extern "C" __declspec(dllexport) void DummyExport() {}
BOOL WINAPI DllMain(HMODULE, DWORD, void*) { return TRUE; }
