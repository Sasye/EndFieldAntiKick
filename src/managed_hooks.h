#pragma once

static bool HideEnabled();
static void BeginRuntimeStop();
static APH_Result APH_CALL OnUpdate(APH_Owner, const APH_SharedEvent*, void*);
static bool EnableOpen(uint64_t);
static ULONGLONG g_enableDeadline = 0;

template<class F> static APH_Result AwaitHook(uint64_t epoch, F advance) {
  for (;;) {
    if (!EnableOpen(epoch)) return APH_E_REVOKED;
    APH_Result result = advance();
    if (result != APH_PENDING && result != APH_E_BUSY) return result;
    if (GetTickCount64() >= g_enableDeadline) return APH_E_UNAVAILABLE;
    if (WaitForSingleObject(g_cancel, 5) != WAIT_TIMEOUT) return APH_E_REVOKED;
  }
}

static bool ShouldHide(LPCWSTR name) {
  if (!name) return false;
  constexpr wchar_t needle[] = L"anti_afk.dll";
  for (; *name; ++name) {
    unsigned i = 0;
    for (; needle[i]; ++i) {
      wchar_t c = name[i];
      if (c >= L'A' && c <= L'Z') c += L'a' - L'A';
      if (c != needle[i]) break;
    }
    if (!needle[i]) return true;
  }
  return false;
}

// NativeHook does not promise LastError preservation. This POD-only exact
// wrapper preserves both the original's INPUT error and its final OUTPUT error
// across enter_call, filtering and leave_call. No allocation, logging or I/O in
// the wrapper (Core itself opens files while scanning/loading/logging).
template<unsigned Tag, class Signature> struct FileHook;
template<unsigned Tag, class R, class... Args>
struct FileHook<Tag, R(WINAPI*)(LPCWSTR, Args...)> {
  using Function = R(WINAPI*)(LPCWSTR, Args...);
  inline static applepie::Hook hook{};
  inline static APH_NativeProc original = nullptr;
  static R Failure() {
    if constexpr (std::is_pointer_v<R>) return INVALID_HANDLE_VALUE;
    else return INVALID_FILE_ATTRIBUTES;
  }
  static R WINAPI Entry(LPCWSTR name, Args... args) {
    const DWORD inputError = GetLastError();
    DWORD outputError = inputError;
    APH_HookCall call{};
    const APH_Result entered = hook.api()->enter_call(hook.owner(), hook.handle(), &call);
    unsigned phase = APH_ORIGINAL_NOT_ENTERED;
    R result{};
    __try {
      __try {
        if (entered == APH_OK && (call.flags & APH_CALL_BUSINESS) && HideEnabled() && ShouldHide(name)) {
          result = Failure();
          outputError = ERROR_FILE_NOT_FOUND;
        } else {
          SetLastError(inputError);
          phase = APH_ORIGINAL_RUNNING;
          result = ((Function)original)(name, args...);
          outputError = GetLastError();
          phase = APH_ORIGINAL_RETURNED;
        }
      } __except (APH_EntryFilter(hook.api(), hook.owner(), hook.handle(),
                                  GetExceptionInformation(), phase, APH_RECOVERY_NONE)) {}
    } __finally {
      if (entered == APH_OK) hook.api()->leave_call(hook.owner(), &call);
      SetLastError(outputError);
    }
    return result;
  }
  static APH_Result Install(uint64_t epoch, Function target, const char* signature, const char* label) {
    APH_HookRequest request{};
    APH_Result r = applepie::MakeHookRequest(request, Tag, (APH_NativeProc)target,
                                            (APH_NativeProc)Entry, signature, label);
    if (r != APH_OK) return r;
    r = hook.Request(g_client, request);
    if (r < 0) return r;
    return AwaitHook(epoch, [] { return hook.Advance(&original); });
  }
};
using CreateFileHook = FileHook<1, decltype(&CreateFileW)>;
using AttributesHook = FileHook<2, decltype(&GetFileAttributesW)>;

// Same zero-payload v1 original-once stop contracts as EIEM and CombatHUD.
// Resident observation remains necessary while this plugin is disabled.
static APH_Result APH_CALL ObserveStop(APH_Owner, const APH_SharedEvent* event, void*) {
  if (event->phase == APH_BEFORE) BeginRuntimeStop();
  return APH_OK;
}
template<unsigned Tag, class... Args> struct StopHook {
  inline static applepie::SharedHook hook{};
  inline static applepie::Subscription observer{};
  inline static APH_NativeProc original = nullptr;
  static int Fault(EXCEPTION_POINTERS* e, unsigned phase) {
    hook.api()->native_fault(hook.owner(), hook.handle(), e->ExceptionRecord->ExceptionCode,
                            (uint64_t)(uintptr_t)e->ExceptionRecord->ExceptionAddress, phase);
    return EXCEPTION_CONTINUE_SEARCH;
  }
  static void Publish(const APH_SharedCall* call, unsigned phase) {
    if (hook.api()->publish(call, phase, nullptr, 0) < 0)
      RaiseException(0xe0411001u, EXCEPTION_NONCONTINUABLE, 0, nullptr);
  }
  static void Entry(Args... args) {
    APH_SharedCall call{};
    const APH_Result entered = hook.api()->enter(hook.owner(), hook.handle(), &call);
    unsigned phase = APH_ORIGINAL_NOT_ENTERED;
    __try {
      __try {
        if (entered == APH_OK) Publish(&call, APH_BEFORE);
        // A provider's own stop guard must survive owner quarantine as well.
        BeginRuntimeStop();
        phase = APH_ORIGINAL_RUNNING;
        ((void(*)(Args...))original)(args...);
        phase = APH_ORIGINAL_RETURNED;
        if (entered == APH_OK) Publish(&call, APH_AFTER);
      } __except (Fault(GetExceptionInformation(), phase)) {}
    } __finally { if (entered == APH_OK) hook.api()->leave(&call); }
  }
  static APH_Result Install(uint64_t epoch, APH_NativeProc target, const char* signature, const char* contract) {
    APH_SharedRequest request{};
    APH_Result r = applepie::MakeSharedRequest(request, Tag, target, (APH_NativeProc)Entry,
                                              signature, contract, 1, 0);
    if (r != APH_OK) return r;
    r = hook.Request(g_client, request);
    if (r < 0) return r;
    if (!observer.handle()) {
      const APH_SharedListen desc{sizeof(desc), APH_BEFORE, APH_MODULE_OBSERVER,
                                  APH_RECOVERY_NONE, ObserveStop, nullptr};
      r = observer.Listen(hook, desc);
      if (r != APH_OK) return r;
    }
    return AwaitHook(epoch, [] { return hook.Advance(&original); });
  }
};
using ShutdownHook = StopHook<3>;
using QuitHook = StopHook<4, void*>;

struct UpdatePayload { void* self; void* method; };
static_assert(sizeof(UpdatePayload) == 16, "GameApp.Update shared payload v1");
struct UpdateHook {
  inline static applepie::SharedHook hook{};
  inline static applepie::Subscription listener{};
  inline static APH_NativeProc original = nullptr;
  static int Fault(EXCEPTION_POINTERS* e, unsigned phase) {
    hook.api()->native_fault(hook.owner(), hook.handle(), e->ExceptionRecord->ExceptionCode,
                            (uint64_t)(uintptr_t)e->ExceptionRecord->ExceptionAddress, phase);
    return EXCEPTION_CONTINUE_SEARCH;
  }
  static void Publish(const APH_SharedCall* call, unsigned phase, const UpdatePayload& p) {
    if (hook.api()->publish(call, phase, &p, sizeof(p)) < 0)
      RaiseException(0xe0411001u, EXCEPTION_NONCONTINUABLE, 0, nullptr);
  }
  static void Entry(void* self, void* method) {
    const UpdatePayload payload{self, method};
    APH_SharedCall call{};
    const APH_Result entered = hook.api()->enter(hook.owner(), hook.handle(), &call);
    unsigned phase = APH_ORIGINAL_NOT_ENTERED;
    __try {
      __try {
        if (entered == APH_OK) Publish(&call, APH_BEFORE, payload);
        phase = APH_ORIGINAL_RUNNING;
        ((void(*)(void*, void*))original)(self, method);
        phase = APH_ORIGINAL_RETURNED;
        if (entered == APH_OK) Publish(&call, APH_AFTER, payload);
      } __except (Fault(GetExceptionInformation(), phase)) {}
    } __finally { if (entered == APH_OK) hook.api()->leave(&call); }
  }
  static APH_Result Install(uint64_t epoch, APH_NativeProc target) {
    APH_SharedRequest request{};
    APH_Result r = applepie::MakeSharedRequest(request, 5, target, (APH_NativeProc)Entry,
        "void(self*,MethodInfo*)", "endfield.GameApp.Update", 1, sizeof(UpdatePayload));
    if (r != APH_OK) return r;
    r = hook.Request(g_client, request);
    if (r < 0) return r;
    r = AwaitHook(epoch, [] { return listener.Close(); });
    if (r != APH_OK) return r;
    const APH_SharedListen desc{sizeof(desc), APH_AFTER, APH_OBSERVER, APH_RECOVERY_NONE, OnUpdate, nullptr};
    r = listener.Listen(hook, desc);
    if (r != APH_OK) return r;
    return AwaitHook(epoch, [] { return hook.Advance(&original); });
  }
};
