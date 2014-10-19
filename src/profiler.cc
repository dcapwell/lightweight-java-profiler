#include "profiler.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "display.h"

#ifdef __APPLE__
// See comment in Accessors class
pthread_key_t Accessors::key_;
#else
__thread JNIEnv * Accessors::env_;
#endif

ASGCTType Asgct::asgct_;

TraceData Profiler::traces_[kMaxStackTraces];

JVMPI_CallFrame Profiler::frame_buffer_[kMaxStackTraces][kMaxFramesToCapture];

int Profiler::failures_[kNumCallTraceErrors + 1];

namespace {

// Helper class to store and reset errno when in a signal handler.
class ErrnoRaii {
 public:
  ErrnoRaii() { stored_errno_ = errno; }
  ~ErrnoRaii() { errno = stored_errno_; }

 private:
  int stored_errno_;

  DISALLOW_COPY_AND_ASSIGN(ErrnoRaii);
};
}  // namespace

static uint64_t CalculateHash(JVMPI_CallTrace *trace, int skip) {
  // Make hash-value
  uint64_t h = 0;
  for (int i = skip; i < trace->num_frames; i++) {
    h += reinterpret_cast<uintptr_t>(trace->frames[i].method_id);
    h += h << 10;
    h ^= h >> 6;
    h += static_cast<uintptr_t>(trace->frames[i].lineno);
    h += h << 10;
    h ^= h >> 6;
  }
  h += h << 3;
  h ^= h >> 11;
  return h;
}

void Profiler::Handle(int signum, siginfo_t *info, void *context) {
  IMPLICITLY_USE(signum);
  IMPLICITLY_USE(info);
  ErrnoRaii err_storage;  // stores and resets errno

  JNIEnv *env = Accessors::CurrentJniEnv();
  if (env == NULL) {
    // native / JIT / GC thread, which isn't attached to the JVM.
    failures_[0]++;
    return;
  }

  JVMPI_CallTrace trace;
  JVMPI_CallFrame frames[kMaxFramesToCapture];
  // We have to set every byte to 0 instead of just initializing the
  // individual fields, because the structs might be padded, and we
  // use memcmp on it later.  We can't use memset, because it isn't
  // async-safe.
  char *base = reinterpret_cast<char *>(frames);
  for (char *p = base; p < base + sizeof(JVMPI_CallFrame) * kMaxFramesToCapture;
       p++) {
    *p = 0;
  }

  trace.frames = frames;
  trace.env_id = env;

  ASGCTType asgct = Asgct::GetAsgct();
  (*asgct)(&trace, kMaxFramesToCapture, context);

  if (trace.num_frames < 0) {
    int idx = -trace.num_frames;
    if (idx > kNumCallTraceErrors) {
      return;
    }
    failures_[idx]++;
  }

  uint64_t hash_val = CalculateHash(&trace, 0);
  uint64_t idx = hash_val % kMaxStackTraces;

  uint64_t i = idx;

  do {
    intptr_t *count = &(traces_[i].count);
    if (*count == 0 && (NoBarrier_CompareAndSwap(count, 0, 1) == 0)) {
      // memcpy is not async safe
      JVMPI_CallFrame *fb = frame_buffer_[i];
      for (int frame_num = 0; frame_num < trace.num_frames; ++frame_num) {
        base = reinterpret_cast<char *>(&(fb[frame_num]));
        // Make sure the padding is all set to 0.
        for (char *p = base; p < base + sizeof(JVMPI_CallFrame); p++) {
          *p = 0;
        }
        fb[frame_num].lineno = trace.frames[frame_num].lineno;
        fb[frame_num].method_id = trace.frames[frame_num].method_id;
      }

      traces_[i].trace.frames = fb;
      traces_[i].trace.num_frames = trace.num_frames;
      return;
    }

    if ((traces_[i].trace.num_frames == trace.num_frames) &&
        (memcmp(traces_[i].trace.frames, trace.frames,
                sizeof(JVMPI_CallFrame) * kMaxFramesToCapture) == 0)) {
      NoBarrier_AtomicIncrement(&(traces_[i].count), 1);
      return;
    }

    i = (i + 1) % kMaxStackTraces;
  } while (i != idx);
}

// This method schedules the SIGPROF timer to go off every sec
// seconds, usec microseconds.
bool SignalHandler::SetSigprofInterval(int sec, int usec) {
  static struct itimerval timer;
  timer.it_interval.tv_sec = sec;
  timer.it_interval.tv_usec = usec;
  timer.it_value = timer.it_interval;
  if (setitimer(ITIMER_PROF, &timer, 0) == -1) {
    fprintf(stderr, "Scheduling profiler interval failed with error %d\n",
            errno);
    return false;
  }
  return true;
}

struct sigaction SignalHandler::SetAction(void (*action)(int, siginfo_t *,
                                                         void *)) {
  struct sigaction sa;
  sa.sa_handler = NULL;
  sa.sa_sigaction = action;
  sa.sa_flags = SA_RESTART | SA_SIGINFO;

  sigemptyset(&sa.sa_mask);

  struct sigaction old_handler;
  if (sigaction(SIGPROF, &sa, &old_handler) != 0) {
    fprintf(stderr, "Scheduling profiler action failed with error %d\n", errno);
    return old_handler;
  }

  return old_handler;
}

bool Profiler::Start() {
  int usec_wait = 1000000 / kNumInterrupts;

  memset(traces_, 0, sizeof(traces_));
  memset(frame_buffer_, 0, sizeof(frame_buffer_));
  memset(failures_, 0, sizeof(failures_));

  // old_action_ is stored, but never used.  This is in case of future
  // refactorings that need it.
  old_action_ = handler_.SetAction(&Profiler::Handle);
  return handler_.SetSigprofInterval(0, usec_wait);
}

void Profiler::Stop() {
  handler_.SetSigprofInterval(0, 0);
  // Breaks encapsulation, but whatever.
  signal(SIGPROF, SIG_IGN);
}

static int CompareTraceData(const void *v1, const void *v2) {
  const TraceData *tr1 = reinterpret_cast<const TraceData *>(v1);
  const TraceData *tr2 = reinterpret_cast<const TraceData *>(v2);
  return tr1->count > tr2->count ? 1 : (tr1->count < tr2->count ? -1 : 0);
}

void Profiler::DumpToFile(FILE *file) {
  qsort(traces_, kMaxStackTraces, sizeof(TraceData), &CompareTraceData);

  StackTracesPrinter printer(file, jvmti_);

  printer.PrintStackTraces(traces_, kMaxStackTraces);
  printer.PrintLeafHistogram(traces_, kMaxStackTraces);

  fprintf(file, "Failures:\n"
                "Instances    Reason\n"
                "%-12d Non Java thread (GC/JIT/pure native)\n"
                "%-12d Stack walking disabled\n"
                "%-12d Java thread doing GC work\n"
                "%-12d In native code, unknown frame.\n"
                "%-12d In native code, non-walkable frame (you are likely to"
                " get this for native code).\n"
                "%-12d In Java code, unknown frame.\n"
                "%-12d In Java code, non-walkable frame (for example,"
                " if the frame is being constructed).\n"
                "%-12d Unknown thread state.\n"
                "%-12d Thread exiting.\n"
                "%-12d Thread in deoptimization"
                " (for dynamic recompilation).\n"
                "%-12d Thread in a safepoint"
                " (such as a stop-the-world GC).\n",
          failures_[-kNativeStackTrace], failures_[-kNoClassLoad],
          failures_[-kGcTraceError], failures_[-kUnknownNotJava],
          failures_[-kNotWalkableFrameNotJava], failures_[-kUnknownJava],
          failures_[-kNotWalkableFrameJava], failures_[-kUnknownState],
          failures_[-kTicksThreadExit], failures_[-kDeoptHandler],
          failures_[-kSafepoint]);
}
