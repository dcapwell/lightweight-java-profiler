#include <signal.h>

#include "globals.h"
#include "stacktraces.h"

#ifndef PROFILER_H
#define PROFILER_H

class SignalHandler {
 public:
  SignalHandler() {}

  struct sigaction SetAction(void (*sigaction)(int, siginfo_t *, void *));

  bool SetSigprofInterval(int sec, int usec);

 private:
  DISALLOW_COPY_AND_ASSIGN(SignalHandler);
};

struct TraceData {
  intptr_t count;
  JVMPI_CallTrace trace;
};

class Profiler {
 public:
  explicit Profiler(jvmtiEnv *jvmti) : jvmti_(jvmti) {}

  bool Start();

  void Stop();

  void DumpToFile(FILE *file);

 private:
  jvmtiEnv *jvmti_;

  SignalHandler handler_;

  struct sigaction old_action_;

  static TraceData traces_[kMaxStackTraces];

  static JVMPI_CallFrame frame_buffer_[kMaxStackTraces][kMaxFramesToCapture];

  static int failures_[kNumCallTraceErrors + 1];  // they are indexed from 1

  static void Handle(int signum, siginfo_t *info, void *context);

  DISALLOW_COPY_AND_ASSIGN(Profiler);
};

#endif  // PROFILER_H
