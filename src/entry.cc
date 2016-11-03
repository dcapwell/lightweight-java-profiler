#include <stdio.h>
#include <limits.h>
#include <string.h>
#include <unistd.h>

#include <string>

#include "globals.h"
#include "profiler.h"
#include "stacktraces.h"

static Profiler *prof;
FILE *Globals::OutFile;

void JNICALL OnThreadStart(jvmtiEnv *jvmti_env, JNIEnv *jni_env,
                           jthread thread) {
  IMPLICITLY_USE(jvmti_env);
  IMPLICITLY_USE(thread);
  Accessors::SetCurrentJniEnv(jni_env);
}

void JNICALL OnThreadEnd(jvmtiEnv *jvmti_env, JNIEnv *jni_env, jthread thread) {
  IMPLICITLY_USE(jvmti_env);
  IMPLICITLY_USE(jni_env);
  IMPLICITLY_USE(thread);
}

// This has to be here, or the VM turns off class loading events.
// And AsyncGetCallTrace needs class loading events to be turned on!
void JNICALL OnClassLoad(jvmtiEnv *jvmti_env, JNIEnv *jni_env, jthread thread,
                         jclass klass) {
  IMPLICITLY_USE(jvmti_env);
  IMPLICITLY_USE(jni_env);
  IMPLICITLY_USE(thread);
  IMPLICITLY_USE(klass);
}

// Calls GetClassMethods on a given class to force the creation of
// jmethodIDs of it.
void CreateJMethodIDsForClass(jvmtiEnv *jvmti, jclass klass) {
  jint method_count;
  JvmtiScopedPtr<jmethodID> methods(jvmti);
  jvmtiError e = jvmti->GetClassMethods(klass, &method_count, methods.GetRef());
  if (e != JVMTI_ERROR_NONE && e != JVMTI_ERROR_CLASS_NOT_PREPARED) {
    // JVMTI_ERROR_CLASS_NOT_PREPARED is okay because some classes may
    // be loaded but not prepared at this point.
    JvmtiScopedPtr<char> ksig(jvmti);
    JVMTI_ERROR((jvmti->GetClassSignature(klass, ksig.GetRef(), NULL)));
    fprintf(
        stderr,
        "Failed to create method IDs for methods in class %s with error %d ",
        ksig.Get(), e);
  }
}

void JNICALL OnVMInit(jvmtiEnv *jvmti, JNIEnv *jni_env, jthread thread) {
  IMPLICITLY_USE(thread);
  IMPLICITLY_USE(jni_env);
  // Forces the creation of jmethodIDs of the classes that had already
  // been loaded (eg java.lang.Object, java.lang.ClassLoader) and
  // OnClassPrepare() misses.
  jint class_count;
  JvmtiScopedPtr<jclass> classes(jvmti);
  JVMTI_ERROR((jvmti->GetLoadedClasses(&class_count, classes.GetRef())));
  jclass *classList = classes.Get();
  for (int i = 0; i < class_count; ++i) {
    jclass klass = classList[i];
    CreateJMethodIDsForClass(jvmti, klass);
  }
  prof->Start();
}

void JNICALL OnClassPrepare(jvmtiEnv *jvmti_env, JNIEnv *jni_env,
                            jthread thread, jclass klass) {
  IMPLICITLY_USE(jni_env);
  IMPLICITLY_USE(thread);
  // We need to do this to "prime the pump", as it were -- make sure
  // that all of the methodIDs have been initialized internally, for
  // AsyncGetCallTrace.  I imagine it slows down class loading a mite,
  // but honestly, how fast does class loading have to be?
  CreateJMethodIDsForClass(jvmti_env, klass);
}

void JNICALL OnVMDeath(jvmtiEnv *jvmti_env, JNIEnv *jni_env) {
  IMPLICITLY_USE(jvmti_env);
  IMPLICITLY_USE(jni_env);

  prof->Stop();
  prof->DumpToFile(Globals::OutFile);
}

static bool PrepareJvmti(jvmtiEnv *jvmti) {
  // Set the list of permissions to do the various internal VM things
  // we want to do.
  jvmtiCapabilities caps;

  memset(&caps, 0, sizeof(caps));
  caps.can_generate_all_class_hook_events = 1;

  caps.can_get_source_file_name = 1;
  caps.can_get_line_numbers = 1;
  caps.can_get_bytecodes = 1;
  caps.can_get_constant_pool = 1;

  jvmtiCapabilities all_caps;
  int error;

  if (JVMTI_ERROR_NONE ==
      (error = jvmti->GetPotentialCapabilities(&all_caps))) {
    // This makes sure that if we need a capability, it is one of the
    // potential capabilities.  The technique isn't wonderful, but it
    // is compact and as likely to be compatible between versions as
    // anything else.
    char *has = reinterpret_cast<char *>(&all_caps);
    const char *should_have = reinterpret_cast<const char *>(&caps);
    for (int i = 0; i < sizeof(all_caps); i++) {
      if ((should_have[i] != 0) && (has[i] == 0)) {
        return false;
      }
    }

    // This adds the capabilities.
    if ((error = jvmti->AddCapabilities(&caps)) != JVMTI_ERROR_NONE) {
      fprintf(stderr, "Failed to add capabilities with error %d\n", error);
      return false;
    }
  }
  return true;
}

static bool RegisterJvmti(jvmtiEnv *jvmti) {
  // Create the list of callbacks to be called on given events.
  jvmtiEventCallbacks *callbacks = new jvmtiEventCallbacks();
  memset(callbacks, 0, sizeof(jvmtiEventCallbacks));

  callbacks->ThreadStart = &OnThreadStart;
  callbacks->ThreadEnd = &OnThreadEnd;
  callbacks->VMInit = &OnVMInit;
  callbacks->VMDeath = &OnVMDeath;

  callbacks->ClassLoad = &OnClassLoad;
  callbacks->ClassPrepare = &OnClassPrepare;

  JVMTI_ERROR_1(
      (jvmti->SetEventCallbacks(callbacks, sizeof(jvmtiEventCallbacks))),
      false);

  jvmtiEvent events[] = {JVMTI_EVENT_CLASS_LOAD, JVMTI_EVENT_CLASS_PREPARE,
                         JVMTI_EVENT_THREAD_END, JVMTI_EVENT_THREAD_START,
                         JVMTI_EVENT_VM_DEATH, JVMTI_EVENT_VM_INIT};

  size_t num_events = sizeof(events) / sizeof(jvmtiEvent);

  // Enable the callbacks to be triggered when the events occur.
  // Events are enumerated in jvmstatagent.h
  for (int i = 0; i < num_events; i++) {
    JVMTI_ERROR_1(
        (jvmti->SetEventNotificationMode(JVMTI_ENABLE, events[i], NULL)),
        false);
  }

  return true;
}

#define POSITIVE(x) (static_cast<size_t>(x > 0 ? x : 0))

static void SetFileFromOption(char *equals) {
  char *name_begin = equals + 1;
  char *name_end;
  if ((name_end = strchr(equals, ',')) == NULL) {
    name_end = equals + strlen(equals);
  }
  size_t len = POSITIVE(name_end - name_begin);
  char *file_name = new char[len];
  for(int i = 0; i < len; ++i){
    file_name[i] = '\0';
  }
  strcpy(file_name, name_begin);
  if (strcmp(file_name, "stderr") == 0) {
    Globals::OutFile = stderr;
  } else if (strcmp(file_name, "stdout") == 0) {
    Globals::OutFile = stdout;
  } else {
    Globals::OutFile = fopen(file_name, "w+");
    if (Globals::OutFile == NULL) {
      fprintf(stderr, "Could not open file %s: ", file_name);
      perror(NULL);
      exit(1);
    }
  }

  delete[] file_name;
}

static void ParseArguments(char *options) {
  char *key = options;
  for (char *next = options; next != NULL;
       next = strchr((key = next + 1), ',')) {
    char *equals = strchr(key, '=');
    if (equals == NULL) {
      fprintf(stderr, "No value for key %s\n", key);
      continue;
    }
    if (strncmp(key, "file", POSITIVE(equals - key)) == 0) {
      SetFileFromOption(equals);
    }
  }

  if (Globals::OutFile == NULL) {
    char path[PATH_MAX];
    if (getcwd(path, PATH_MAX) == NULL) {
      fprintf(stderr, "cwd too long?\n");
      exit(0);
    }
    size_t pathlen = strlen(path);
    strncat(path, "/", PATH_MAX - (pathlen++));
    strncat(path, kDefaultOutFile, PATH_MAX - pathlen);
    Globals::OutFile = fopen(path, "w+");
  }
}

AGENTEXPORT jint JNICALL Agent_OnLoad(JavaVM *vm, char *options,
                                      void *reserved) {
  IMPLICITLY_USE(reserved);
  int err;
  jvmtiEnv *jvmti;
  ParseArguments(options);

  Accessors::Init();

  if ((err = (vm->GetEnv(reinterpret_cast<void **>(&jvmti), JVMTI_VERSION))) !=
      JNI_OK) {
    fprintf(stderr, "JNI Error %d\n", err);
    return 1;
  }

  if (!PrepareJvmti(jvmti)) {
    fprintf(stderr, "Failed to initialize JVMTI.  Continuing...\n");
    return 0;
  }

  if (!RegisterJvmti(jvmti)) {
    fprintf(stderr, "Failed to enable JVMTI events.  Continuing...\n");
    // We fail hard here because we may have failed in the middle of
    // registering callbacks, which will leave the system in an
    // inconsistent state.
    return 1;
  }

  Asgct::SetAsgct(Accessors::GetJvmFunction<ASGCTType>("AsyncGetCallTrace"));

  prof = new Profiler(jvmti);

  return 0;
}

AGENTEXPORT void JNICALL Agent_OnUnload(JavaVM *vm) {
  IMPLICITLY_USE(vm);
  Accessors::Destroy();
}
