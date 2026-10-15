#pragma once

// The worker discovers only non-generic lifecycle metadata. Network discovery
// and gameplay calls wait for GameState.Main on the validated game thread.
struct Il2CppApi {
  void* (*get_corlib)();
  void** (*thread_get_all_attached_threads)(size_t*);
  void* (*domain_get)();
  void* (*thread_attach)(void*);
  void (*thread_detach)(void*);
  void* (*thread_current)();
  void** (*domain_get_assemblies)(void*, size_t*);
  void* (*assembly_get_image)(void*);
  void* (*class_from_name)(void*, const char*, const char*);
  void* (*class_get_parent)(void*);
  void* (*class_get_methods)(void*, void**);
  void* (*class_get_field_from_name)(void*, const char*);
  void* (*field_get_type)(void*);
  uint32_t (*field_get_flags)(void*);
  void (*field_static_get_value)(void*, void*);
  void (*field_set_value)(void*, void*, void*);
  const char* (*method_get_name)(void*);
  uint32_t (*method_get_param_count)(void*);
  uint32_t (*method_get_flags)(void*, uint32_t*);
  bool (*method_is_generic)(void*);
  void* (*method_get_return_type)(void*);
  void* (*method_get_param)(void*, uint32_t);
  bool (*type_is_byref)(void*);
  char* (*type_get_name)(void*);
  void (*free)(void*);
  void* (*class_from_type)(void*);
  bool (*class_is_valuetype)(void*);
  bool (*class_is_enum)(void*);
  bool (*class_is_assignable_from)(void*, void*);
  void* (*object_get_class)(void*);
  void* (*object_new)(void*);
  void* (*object_unbox)(void*);
  void* (*runtime_invoke)(void*, void*, void**, void**);
  uint32_t (*gchandle_new)(void*, bool);
  void (*gchandle_free)(uint32_t);
};
static Il2CppApi g_il{};
static HMODULE g_gameAssembly = nullptr;
static bool RuntimeOpen();
static void DebugLog(const char*, ...);
static void DiagnosticLog(const char*, ...);

static bool ResolveIl2Cpp() {
  HMODULE module = GetModuleHandleW(L"GameAssembly.dll");
  if (!module) return false;
  Il2CppApi api{};
#define RESOLVE(name) \
  api.name = (decltype(api.name))GetProcAddress(module, "il2cpp_" #name); \
  if (!api.name) return false
  RESOLVE(get_corlib); RESOLVE(thread_get_all_attached_threads);
  RESOLVE(domain_get); RESOLVE(thread_attach); RESOLVE(thread_detach);
  RESOLVE(thread_current); RESOLVE(domain_get_assemblies);
  RESOLVE(assembly_get_image); RESOLVE(class_from_name); RESOLVE(class_get_parent);
  RESOLVE(class_get_methods); RESOLVE(class_get_field_from_name);
  RESOLVE(field_get_type); RESOLVE(field_get_flags); RESOLVE(field_static_get_value); RESOLVE(field_set_value);
  RESOLVE(method_get_name); RESOLVE(method_get_param_count); RESOLVE(method_get_flags);
  RESOLVE(method_is_generic); RESOLVE(method_get_return_type); RESOLVE(method_get_param);
  RESOLVE(type_is_byref); RESOLVE(type_get_name); RESOLVE(free); RESOLVE(class_from_type);
  RESOLVE(class_is_valuetype); RESOLVE(class_is_enum); RESOLVE(class_is_assignable_from);
  RESOLVE(object_get_class); RESOLVE(object_new); RESOLVE(object_unbox);
  RESOLVE(runtime_invoke); RESOLVE(gchandle_new); RESOLVE(gchandle_free);
#undef RESOLVE
  // Pin exports/metadata for the process lifetime, just like the native claims.
  HMODULE pinned = nullptr;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                         (LPCWSTR)api.domain_get, &pinned)) return false;
  g_gameAssembly = module;
  g_il = api;
  return true;
}

static bool WorkerAttachReady() {
  // domain_get can allocate the domain and its managed AppDomain. It is NOT a
  // readiness probe. In this IL2CPP startup order corlib follows GC/Thread init;
  // the first registered VM thread follows creation of the domain and classes.
  if (!RuntimeOpen() || !g_il.get_corlib || !g_il.thread_get_all_attached_threads || !g_il.get_corlib()) return false;
  size_t count = 0;
  // Only inspect presence/count; never traverse the borrowed, mutable array.
  return g_il.thread_get_all_attached_threads(&count) && count != 0;
}

static bool TypeIs(void* type, const char* expected) {
  if (!type || g_il.type_is_byref(type)) return false;
  char* name = g_il.type_get_name(type);
  if (!name) return false;
  // IL2CPP versions spell nested types with '.', '/' or '+'.
  for (char* p = name; *p; ++p) if (*p == '/' || *p == '+') *p = '.';
  bool same = strcmp(name, expected) == 0;
  g_il.free(name);
  return same;
}

static bool ReferenceType(void* type) {
  if (!type || g_il.type_is_byref(type)) return false;
  void* klass = g_il.class_from_type(type);
  return klass && !g_il.class_is_valuetype(klass);
}

static bool MethodIs(void* method, bool isStatic, const char* result, const char* parameter = nullptr) {
  if (!method || g_il.method_is_generic(method)) return false;
  uint32_t impl = 0;
  uint32_t flags = g_il.method_get_flags(method, &impl);
  return bool(flags & 0x10u) == isStatic && !(flags & 0x400u) &&
    g_il.method_get_param_count(method) == (parameter ? 1u : 0u) &&
    TypeIs(g_il.method_get_return_type(method), result) &&
    (!parameter || TypeIs(g_il.method_get_param(method, 0), parameter));
}

static void* FindDeclaredMethod(void* klass, const char* name, bool isStatic,
                               const char* result, const char* parameter = nullptr, bool* ambiguous = nullptr) {
  if (!klass) return nullptr;
  void* iterator = nullptr;
  void* match = nullptr;
  while (void* method = g_il.class_get_methods(klass, &iterator)) {
    if (strcmp(g_il.method_get_name(method), name) == 0 &&
        MethodIs(method, isStatic, result, parameter)) {
      if (match) {
        if (ambiguous) *ambiguous = true;
        return nullptr; // Ambiguous overloads are not a verified ABI.
      }
      match = method;
    }
  }
  return match;
}

static void* FindMethod(void* klass, const char* name, bool isStatic,
                        const char* result, const char* parameter = nullptr) {
  for (; klass; klass = g_il.class_get_parent(klass)) {
    bool ambiguous = false;
    void* match = FindDeclaredMethod(klass, name, isStatic, result, parameter, &ambiguous);
    if (ambiguous) return nullptr;
    if (match) return match;
  }
  return nullptr;
}

static void* FindClass(void** assemblies, size_t count, const char* ns, const char* name) {
  for (size_t i = 0; i < count && RuntimeOpen(); ++i) {
    void* image = g_il.assembly_get_image(assemblies[i]);
    void* klass = image ? g_il.class_from_name(image, ns, name) : nullptr;
    if (klass) return klass;
  }
  return nullptr;
}

struct GameAppMetadata {
  void* update;
  void* quit;
  void* currentState;
  int32_t mainState;
};
static GameAppMetadata g_app{};

static bool ResolveLifecycleMetadata(void** assemblies, size_t count) {
  GameAppMetadata a{};
  void* app = FindClass(assemblies, count, "Beyond", "GameApp");
  a.update = FindDeclaredMethod(app, "Update", false, "System.Void");
  a.currentState = FindDeclaredMethod(app, "get_currentState", false, "Beyond.GameState");
  void* unity = FindClass(assemblies, count, "UnityEngine", "Application");
  a.quit = FindDeclaredMethod(unity, "Internal_ApplicationQuit", true, "System.Void");
  if (!a.update || !a.currentState || !a.quit) return false;
  void* state = g_il.class_from_type(g_il.method_get_return_type(a.currentState));
  if (!state || !g_il.class_is_enum(state)) return false;
  void* storage = g_il.class_get_field_from_name(state, "value__");
  void* main = g_il.class_get_field_from_name(state, "Main");
  if (!storage || (g_il.field_get_flags(storage) & 0x10u) ||
      !TypeIs(g_il.field_get_type(storage), "System.Int32") || !main ||
      (g_il.field_get_flags(main) & 0x50u) != 0x50u ||
      g_il.class_from_type(g_il.field_get_type(main)) != state) return false;
  g_il.field_static_get_value(main, &a.mainState);
  g_app = a;
  return true;
}

struct ProtoMessage { void* klass; void* ctor; void* timestamp; const char* name; };
struct NetworkMetadata {
  void* netBus;
  void* instanceField;
  void* sender;
  void* validateSession;
  void* sessionId;
  const char* messageInterface;
  void* messageClass;
  ProtoMessage messages[3];
};
static NetworkMetadata g_net{};

static bool NetworkMetadataError(const char* reason) {
  DiagnosticLog("Network metadata unsupported: %s", reason);
  return false;
}

static void* FindSenderSend(void* senderClass, const char* messageInterface) {
  void* send = FindMethod(senderClass, "Send", false, "Beyond.Network.NetBus.ResultHandler", messageInterface);
  return send && ReferenceType(g_il.method_get_return_type(send)) &&
    ReferenceType(g_il.method_get_param(send, 0)) ? send : nullptr;
}

// Call only after GameState.Main is confirmed in a due GameApp.Update callback.
static bool ResolveNetworkMetadata(void** assemblies, size_t count) {
  NetworkMetadata n{};
  // Preserve the historical namespace fallback, but validate the concrete type.
  const char* namespaces[] = {"Beyond.Network", "Beyond", ""};
  for (auto ns : namespaces) {
    n.netBus = FindClass(assemblies, count, ns, "NetBus");
    if (n.netBus) break;
  }
  if (!n.netBus) return NetworkMetadataError("NetBus class not found");
  // Read the initialized singleton field; do not enumerate or invoke methods
  // on ExplicitSingleton<NetBus>. Its generic method setup is unsafe during
  // early startup in the affected game build (OnInit/get_instance AOT errors).
  for (void* klass = n.netBus; klass; klass = g_il.class_get_parent(klass)) {
    void* field = g_il.class_get_field_from_name(klass, "<instance>k__BackingField");
    if (!field) continue;
    void* type = g_il.field_get_type(field);
    if ((g_il.field_get_flags(field) & 0x50u) != 0x10u || !ReferenceType(type) ||
        g_il.class_from_type(type) != n.netBus)
      return NetworkMetadataError("NetBus singleton field flags/type mismatch");
    n.instanceField = field;
    break;
  }
  n.sender = FindDeclaredMethod(n.netBus, "get_defaultSender", false,
                        "Beyond.Network.DisposableBusSender");
  n.validateSession = FindDeclaredMethod(n.netBus, "_ValidateSession", false, "System.Boolean");
  n.sessionId = FindDeclaredMethod(n.netBus, "get_currentSessionID", false, "System.Int32");
  if (!n.instanceField) return NetworkMetadataError("NetBus singleton field missing");
  if (!n.sender || !ReferenceType(g_il.method_get_return_type(n.sender)))
    return NetworkMetadataError("NetBus.get_defaultSender signature mismatch");
  if (!n.validateSession) return NetworkMetadataError("NetBus._ValidateSession signature mismatch");
  if (!n.sessionId) return NetworkMetadataError("NetBus.get_currentSessionID signature mismatch");

  // The October update uses protobuf-net. Select by the exact Send signature,
  // not by whether a protobuf assembly happens to be loaded in the process.
  void* senderClass = g_il.class_from_type(g_il.method_get_return_type(n.sender));
  const char* interfaces[] = {"ProtoBuf.IExtensible", "Google.Protobuf.IMessage"};
  for (const char* name : interfaces) {
    void* send = FindSenderSend(senderClass, name);
    if (!send) continue;
    if (n.messageInterface) return NetworkMetadataError("Sender.Send has multiple supported message interfaces");
    n.messageInterface = name;
    n.messageClass = g_il.class_from_type(g_il.method_get_param(send, 0));
  }
  if (!n.messageInterface) return NetworkMetadataError("Sender.Send has no supported message interface/signature");

  const char* names[] = {"CS_FRIEND_LIST_SIMPLE_SYNC", "CS_SCENE_REST", "CS_PING"};
  bool found = false;
  for (unsigned i = 0; i < 3; ++i) {
    ProtoMessage msg{};
    msg.name = names[i];
    msg.klass = FindClass(assemblies, count, "Proto", names[i]);
    if (!msg.klass) continue;
    msg.ctor = FindDeclaredMethod(msg.klass, ".ctor", false, "System.Void");
    if (!msg.ctor || g_il.class_is_valuetype(msg.klass) ||
        !g_il.class_is_assignable_from(n.messageClass, msg.klass)) {
      DiagnosticLog("Skipping %s: constructor or %s implementation mismatch", msg.name, n.messageInterface);
      continue;
    }
    if (i == 2) {
      const char* timestampField = !strcmp(n.messageInterface, "ProtoBuf.IExtensible")
        ? "<ClientTs>k__BackingField" : "clientTs_";
      msg.timestamp = g_il.class_get_field_from_name(msg.klass, timestampField);
      if (!msg.timestamp || (g_il.field_get_flags(msg.timestamp) & 0x10u) ||
          !TypeIs(g_il.field_get_type(msg.timestamp), "System.UInt64")) {
        DiagnosticLog("Skipping %s: %s is not an instance UInt64 field", msg.name, timestampField);
        continue;
      }
    }
    // A broken optional fallback must not invalidate a verified earlier message.
    n.messages[i] = msg;
    found = true;
  }
  if (!found) return NetworkMetadataError("No compatible keepalive message");
  g_net = n;
  DiagnosticLog("Network API resolved: Sender.Send(%s)", n.messageInterface);
  return true;
}

static void* ReadNetBus() {
  if (!RuntimeOpen() || !g_net.instanceField) return nullptr;
  void* bus = nullptr;
  g_il.field_static_get_value(g_net.instanceField, &bus);
  return RuntimeOpen() ? bus : nullptr;
}

// Metadata MethodInfo starts with methodPointer in the supported IL2CPP build.
// Only used after checking static/instance, arity, types and non-generic method.
static APH_NativeProc NativeAddress(void* method) {
  return method ? *(APH_NativeProc*)method : nullptr;
}

static bool Invoke(void* method, void* object, void** args, void** result) {
  if (!RuntimeOpen()) return false;
  void* exception = nullptr;
  void* value = g_il.runtime_invoke(method, object, args, &exception);
  if (exception) { DebugLog("Managed exception in %s; operation not replayed", g_il.method_get_name(method)); return false; }
  if (result) *result = value;
  return RuntimeOpen();
}

template<class T> static bool InvokeValue(void* method, void* object, T& out) {
  void* boxed = nullptr;
  if (!Invoke(method, object, nullptr, &boxed) || !boxed) return false;
  void* data = g_il.object_unbox(boxed);
  if (!data) return false;
  memcpy(&out, data, sizeof(out));
  return true;
}
