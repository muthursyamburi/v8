// Copyright 2011 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/log.h"

#include <cstdarg>
#include <memory>
#include <sstream>

#include "src/api.h"
#include "src/bailout-reason.h"
#include "src/base/platform/platform.h"
#include "src/bootstrapper.h"
#include "src/code-stubs.h"
#include "src/counters.h"
#include "src/deoptimizer.h"
#include "src/global-handles.h"
#include "src/interpreter/bytecodes.h"
#include "src/interpreter/interpreter.h"
#include "src/libsampler/sampler.h"
#include "src/log-inl.h"
#include "src/macro-assembler.h"
#include "src/perf-jit.h"
#include "src/profiler/profiler-listener.h"
#include "src/profiler/tick-sample.h"
#include "src/runtime-profiler.h"
#include "src/source-position-table.h"
#include "src/string-stream.h"
#include "src/tracing/tracing-category-observer.h"
#include "src/unicode-inl.h"
#include "src/vm-state-inl.h"

#include "src/utils.h"
#include "src/version.h"

namespace v8 {
namespace internal {

#define DECLARE_EVENT(ignore1, name) name,
static const char* kLogEventsNames[CodeEventListener::NUMBER_OF_LOG_EVENTS] = {
    LOG_EVENTS_AND_TAGS_LIST(DECLARE_EVENT)};
#undef DECLARE_EVENT

static const char* ComputeMarker(SharedFunctionInfo* shared,
                                 AbstractCode* code) {
  switch (code->kind()) {
    case AbstractCode::INTERPRETED_FUNCTION:
      return shared->optimization_disabled() ? "" : "~";
    case AbstractCode::OPTIMIZED_FUNCTION:
      return "*";
    default:
      return "";
  }
}


class CodeEventLogger::NameBuffer {
 public:
  NameBuffer() { Reset(); }

  void Reset() {
    utf8_pos_ = 0;
  }

  void Init(CodeEventListener::LogEventsAndTags tag) {
    Reset();
    AppendBytes(kLogEventsNames[tag]);
    AppendByte(':');
  }

  void AppendName(Name* name) {
    if (name->IsString()) {
      AppendString(String::cast(name));
    } else {
      Symbol* symbol = Symbol::cast(name);
      AppendBytes("symbol(");
      if (!symbol->name()->IsUndefined(symbol->GetIsolate())) {
        AppendBytes("\"");
        AppendString(String::cast(symbol->name()));
        AppendBytes("\" ");
      }
      AppendBytes("hash ");
      AppendHex(symbol->Hash());
      AppendByte(')');
    }
  }

  void AppendString(String* str) {
    if (str == nullptr) return;
    int uc16_length = Min(str->length(), kUtf16BufferSize);
    String::WriteToFlat(str, utf16_buffer, 0, uc16_length);
    int previous = unibrow::Utf16::kNoPreviousCharacter;
    for (int i = 0; i < uc16_length && utf8_pos_ < kUtf8BufferSize; ++i) {
      uc16 c = utf16_buffer[i];
      if (c <= unibrow::Utf8::kMaxOneByteChar) {
        utf8_buffer_[utf8_pos_++] = static_cast<char>(c);
      } else {
        int char_length = unibrow::Utf8::Length(c, previous);
        if (utf8_pos_ + char_length > kUtf8BufferSize) break;
        unibrow::Utf8::Encode(utf8_buffer_ + utf8_pos_, c, previous);
        utf8_pos_ += char_length;
      }
      previous = c;
    }
  }

  void AppendBytes(const char* bytes, int size) {
    size = Min(size, kUtf8BufferSize - utf8_pos_);
    MemCopy(utf8_buffer_ + utf8_pos_, bytes, size);
    utf8_pos_ += size;
  }

  void AppendBytes(const char* bytes) {
    AppendBytes(bytes, StrLength(bytes));
  }

  void AppendByte(char c) {
    if (utf8_pos_ >= kUtf8BufferSize) return;
    utf8_buffer_[utf8_pos_++] = c;
  }

  void AppendInt(int n) {
    int space = kUtf8BufferSize - utf8_pos_;
    if (space <= 0) return;
    Vector<char> buffer(utf8_buffer_ + utf8_pos_, space);
    int size = SNPrintF(buffer, "%d", n);
    if (size > 0 && utf8_pos_ + size <= kUtf8BufferSize) {
      utf8_pos_ += size;
    }
  }

  void AppendHex(uint32_t n) {
    int space = kUtf8BufferSize - utf8_pos_;
    if (space <= 0) return;
    Vector<char> buffer(utf8_buffer_ + utf8_pos_, space);
    int size = SNPrintF(buffer, "%x", n);
    if (size > 0 && utf8_pos_ + size <= kUtf8BufferSize) {
      utf8_pos_ += size;
    }
  }

  const char* get() { return utf8_buffer_; }
  int size() const { return utf8_pos_; }

 private:
  static const int kUtf8BufferSize = 512;
  static const int kUtf16BufferSize = kUtf8BufferSize;

  int utf8_pos_;
  char utf8_buffer_[kUtf8BufferSize];
  uc16 utf16_buffer[kUtf16BufferSize];
};


CodeEventLogger::CodeEventLogger() : name_buffer_(new NameBuffer) { }

CodeEventLogger::~CodeEventLogger() { delete name_buffer_; }

void CodeEventLogger::CodeCreateEvent(CodeEventListener::LogEventsAndTags tag,
                                      AbstractCode* code, const char* comment) {
  name_buffer_->Init(tag);
  name_buffer_->AppendBytes(comment);
  LogRecordedBuffer(code, nullptr, name_buffer_->get(), name_buffer_->size());
}

void CodeEventLogger::CodeCreateEvent(CodeEventListener::LogEventsAndTags tag,
                                      AbstractCode* code, Name* name) {
  name_buffer_->Init(tag);
  name_buffer_->AppendName(name);
  LogRecordedBuffer(code, nullptr, name_buffer_->get(), name_buffer_->size());
}

void CodeEventLogger::CodeCreateEvent(CodeEventListener::LogEventsAndTags tag,
                                      AbstractCode* code,
                                      SharedFunctionInfo* shared, Name* name) {
  name_buffer_->Init(tag);
  name_buffer_->AppendBytes(ComputeMarker(shared, code));
  name_buffer_->AppendName(name);
  LogRecordedBuffer(code, shared, name_buffer_->get(), name_buffer_->size());
}

void CodeEventLogger::CodeCreateEvent(CodeEventListener::LogEventsAndTags tag,
                                      AbstractCode* code,
                                      SharedFunctionInfo* shared, Name* source,
                                      int line, int column) {
  name_buffer_->Init(tag);
  name_buffer_->AppendBytes(ComputeMarker(shared, code));
  name_buffer_->AppendString(shared->DebugName());
  name_buffer_->AppendByte(' ');
  if (source->IsString()) {
    name_buffer_->AppendString(String::cast(source));
  } else {
    name_buffer_->AppendBytes("symbol(hash ");
    name_buffer_->AppendHex(Name::cast(source)->Hash());
    name_buffer_->AppendByte(')');
  }
  name_buffer_->AppendByte(':');
  name_buffer_->AppendInt(line);
  LogRecordedBuffer(code, shared, name_buffer_->get(), name_buffer_->size());
}

void CodeEventLogger::RegExpCodeCreateEvent(AbstractCode* code,
                                            String* source) {
  name_buffer_->Init(CodeEventListener::REG_EXP_TAG);
  name_buffer_->AppendString(source);
  LogRecordedBuffer(code, nullptr, name_buffer_->get(), name_buffer_->size());
}


// Linux perf tool logging support
class PerfBasicLogger : public CodeEventLogger {
 public:
  PerfBasicLogger();
  ~PerfBasicLogger() override;

  void CodeMoveEvent(AbstractCode* from, Address to) override {}
  void CodeDisableOptEvent(AbstractCode* code,
                           SharedFunctionInfo* shared) override {}

 private:
  void LogRecordedBuffer(AbstractCode* code, SharedFunctionInfo* shared,
                         const char* name, int length) override;

  // Extension added to V8 log file name to get the low-level log name.
  static const char kFilenameFormatString[];
  static const int kFilenameBufferPadding;

  FILE* perf_output_handle_;
};

const char PerfBasicLogger::kFilenameFormatString[] = "/tmp/perf-%d.map";
// Extra space for the PID in the filename
const int PerfBasicLogger::kFilenameBufferPadding = 16;

PerfBasicLogger::PerfBasicLogger() : perf_output_handle_(nullptr) {
  // Open the perf JIT dump file.
  int bufferSize = sizeof(kFilenameFormatString) + kFilenameBufferPadding;
  ScopedVector<char> perf_dump_name(bufferSize);
  int size = SNPrintF(
      perf_dump_name,
      kFilenameFormatString,
      base::OS::GetCurrentProcessId());
  CHECK_NE(size, -1);
  perf_output_handle_ =
      base::OS::FOpen(perf_dump_name.start(), base::OS::LogFileOpenMode);
  CHECK_NOT_NULL(perf_output_handle_);
  setvbuf(perf_output_handle_, nullptr, _IOLBF, 0);
}


PerfBasicLogger::~PerfBasicLogger() {
  fclose(perf_output_handle_);
  perf_output_handle_ = nullptr;
}

void PerfBasicLogger::LogRecordedBuffer(AbstractCode* code, SharedFunctionInfo*,
                                        const char* name, int length) {
  if (FLAG_perf_basic_prof_only_functions &&
      (code->kind() != AbstractCode::INTERPRETED_FUNCTION &&
       code->kind() != AbstractCode::OPTIMIZED_FUNCTION)) {
    return;
  }

  // Linux perf expects hex literals without a leading 0x, while some
  // implementations of printf might prepend one when using the %p format
  // for pointers, leading to wrongly formatted JIT symbols maps.
  //
  // Instead, we use V8PRIxPTR format string and cast pointer to uintpr_t,
  // so that we have control over the exact output format.
  base::OS::FPrint(perf_output_handle_, "%" V8PRIxPTR " %x %.*s\n",
                   reinterpret_cast<uintptr_t>(code->instruction_start()),
                   code->instruction_size(), length, name);
}

// Low-level logging support.
#define LL_LOG(Call) if (ll_logger_) ll_logger_->Call;

class LowLevelLogger : public CodeEventLogger {
 public:
  explicit LowLevelLogger(const char* file_name);
  ~LowLevelLogger() override;

  void CodeMoveEvent(AbstractCode* from, Address to) override;
  void CodeDisableOptEvent(AbstractCode* code,
                           SharedFunctionInfo* shared) override {}
  void SnapshotPositionEvent(HeapObject* obj, int pos);
  void CodeMovingGCEvent() override;

 private:
  void LogRecordedBuffer(AbstractCode* code, SharedFunctionInfo* shared,
                         const char* name, int length) override;

  // Low-level profiling event structures.
  struct CodeCreateStruct {
    static const char kTag = 'C';

    int32_t name_size;
    Address code_address;
    int32_t code_size;
  };


  struct CodeMoveStruct {
    static const char kTag = 'M';

    Address from_address;
    Address to_address;
  };


  static const char kCodeMovingGCTag = 'G';


  // Extension added to V8 log file name to get the low-level log name.
  static const char kLogExt[];

  void LogCodeInfo();
  void LogWriteBytes(const char* bytes, int size);

  template <typename T>
  void LogWriteStruct(const T& s) {
    char tag = T::kTag;
    LogWriteBytes(reinterpret_cast<const char*>(&tag), sizeof(tag));
    LogWriteBytes(reinterpret_cast<const char*>(&s), sizeof(s));
  }

  FILE* ll_output_handle_;
};

const char LowLevelLogger::kLogExt[] = ".ll";

LowLevelLogger::LowLevelLogger(const char* name) : ll_output_handle_(nullptr) {
  // Open the low-level log file.
  size_t len = strlen(name);
  ScopedVector<char> ll_name(static_cast<int>(len + sizeof(kLogExt)));
  MemCopy(ll_name.start(), name, len);
  MemCopy(ll_name.start() + len, kLogExt, sizeof(kLogExt));
  ll_output_handle_ =
      base::OS::FOpen(ll_name.start(), base::OS::LogFileOpenMode);
  setvbuf(ll_output_handle_, nullptr, _IOLBF, 0);

  LogCodeInfo();
}


LowLevelLogger::~LowLevelLogger() {
  fclose(ll_output_handle_);
  ll_output_handle_ = nullptr;
}


void LowLevelLogger::LogCodeInfo() {
#if V8_TARGET_ARCH_IA32
  const char arch[] = "ia32";
#elif V8_TARGET_ARCH_X64 && V8_TARGET_ARCH_64_BIT
  const char arch[] = "x64";
#elif V8_TARGET_ARCH_X64 && V8_TARGET_ARCH_32_BIT
  const char arch[] = "x32";
#elif V8_TARGET_ARCH_ARM
  const char arch[] = "arm";
#elif V8_TARGET_ARCH_PPC
  const char arch[] = "ppc";
#elif V8_TARGET_ARCH_MIPS
  const char arch[] = "mips";
#elif V8_TARGET_ARCH_ARM64
  const char arch[] = "arm64";
#elif V8_TARGET_ARCH_S390
  const char arch[] = "s390";
#else
  const char arch[] = "unknown";
#endif
  LogWriteBytes(arch, sizeof(arch));
}

void LowLevelLogger::LogRecordedBuffer(AbstractCode* code, SharedFunctionInfo*,
                                       const char* name, int length) {
  CodeCreateStruct event;
  event.name_size = length;
  event.code_address = code->instruction_start();
  event.code_size = code->instruction_size();
  LogWriteStruct(event);
  LogWriteBytes(name, length);
  LogWriteBytes(
      reinterpret_cast<const char*>(code->instruction_start()),
      code->instruction_size());
}

void LowLevelLogger::CodeMoveEvent(AbstractCode* from, Address to) {
  CodeMoveStruct event;
  event.from_address = from->instruction_start();
  size_t header_size = from->instruction_start() - from->address();
  event.to_address = to + header_size;
  LogWriteStruct(event);
}


void LowLevelLogger::LogWriteBytes(const char* bytes, int size) {
  size_t rv = fwrite(bytes, 1, size, ll_output_handle_);
  DCHECK(static_cast<size_t>(size) == rv);
  USE(rv);
}


void LowLevelLogger::CodeMovingGCEvent() {
  const char tag = kCodeMovingGCTag;

  LogWriteBytes(&tag, sizeof(tag));
}

class JitLogger : public CodeEventLogger {
 public:
  explicit JitLogger(JitCodeEventHandler code_event_handler);

  void CodeMoveEvent(AbstractCode* from, Address to) override;
  void CodeDisableOptEvent(AbstractCode* code,
                           SharedFunctionInfo* shared) override {}
  void AddCodeLinePosInfoEvent(void* jit_handler_data, int pc_offset,
                               int position,
                               JitCodeEvent::PositionType position_type);

  void* StartCodePosInfoEvent();
  void EndCodePosInfoEvent(AbstractCode* code, void* jit_handler_data);

 private:
  void LogRecordedBuffer(AbstractCode* code, SharedFunctionInfo* shared,
                         const char* name, int length) override;

  JitCodeEventHandler code_event_handler_;
  base::Mutex logger_mutex_;
};


JitLogger::JitLogger(JitCodeEventHandler code_event_handler)
    : code_event_handler_(code_event_handler) {
}

void JitLogger::LogRecordedBuffer(AbstractCode* code,
                                  SharedFunctionInfo* shared, const char* name,
                                  int length) {
  JitCodeEvent event;
  memset(&event, 0, sizeof(event));
  event.type = JitCodeEvent::CODE_ADDED;
  event.code_start = code->instruction_start();
  event.code_len = code->instruction_size();
  Handle<SharedFunctionInfo> shared_function_handle;
  if (shared && shared->script()->IsScript()) {
    shared_function_handle = Handle<SharedFunctionInfo>(shared);
  }
  event.script = ToApiHandle<v8::UnboundScript>(shared_function_handle);
  event.name.str = name;
  event.name.len = length;
  code_event_handler_(&event);
}

void JitLogger::CodeMoveEvent(AbstractCode* from, Address to) {
  base::LockGuard<base::Mutex> guard(&logger_mutex_);

  JitCodeEvent event;
  event.type = JitCodeEvent::CODE_MOVED;
  event.code_start = from->instruction_start();
  event.code_len = from->instruction_size();

  // Calculate the header size.
  const size_t header_size = from->instruction_start() - from->address();

  // Calculate the new start address of the instructions.
  event.new_code_start = to + header_size;

  code_event_handler_(&event);
}

void JitLogger::AddCodeLinePosInfoEvent(
    void* jit_handler_data,
    int pc_offset,
    int position,
    JitCodeEvent::PositionType position_type) {
  JitCodeEvent event;
  memset(&event, 0, sizeof(event));
  event.type = JitCodeEvent::CODE_ADD_LINE_POS_INFO;
  event.user_data = jit_handler_data;
  event.line_info.offset = pc_offset;
  event.line_info.pos = position;
  event.line_info.position_type = position_type;

  code_event_handler_(&event);
}


void* JitLogger::StartCodePosInfoEvent() {
  JitCodeEvent event;
  memset(&event, 0, sizeof(event));
  event.type = JitCodeEvent::CODE_START_LINE_INFO_RECORDING;

  code_event_handler_(&event);
  return event.user_data;
}

void JitLogger::EndCodePosInfoEvent(AbstractCode* code,
                                    void* jit_handler_data) {
  JitCodeEvent event;
  memset(&event, 0, sizeof(event));
  event.type = JitCodeEvent::CODE_END_LINE_INFO_RECORDING;
  event.code_start = code->instruction_start();
  event.user_data = jit_handler_data;

  code_event_handler_(&event);
}


// TODO(lpy): Keeping sampling thread inside V8 is a workaround currently,
// the reason is to reduce code duplication during migration to sampler library,
// sampling thread, as well as the sampler, will be moved to D8 eventually.
class SamplingThread : public base::Thread {
 public:
  static const int kSamplingThreadStackSize = 64 * KB;

  SamplingThread(sampler::Sampler* sampler, int interval_microseconds)
      : base::Thread(
            base::Thread::Options("SamplingThread", kSamplingThreadStackSize)),
        sampler_(sampler),
        interval_microseconds_(interval_microseconds) {}
  void Run() override {
    while (sampler_->IsProfiling()) {
      sampler_->DoSample();
      base::OS::Sleep(
          base::TimeDelta::FromMicroseconds(interval_microseconds_));
    }
  }

 private:
  sampler::Sampler* sampler_;
  const int interval_microseconds_;
};


// The Profiler samples pc and sp values for the main thread.
// Each sample is appended to a circular buffer.
// An independent thread removes data and writes it to the log.
// This design minimizes the time spent in the sampler.
//
class Profiler: public base::Thread {
 public:
  explicit Profiler(Isolate* isolate);
  void Engage();
  void Disengage();

  // Inserts collected profiling data into buffer.
  void Insert(v8::TickSample* sample) {
    if (paused_)
      return;

    if (Succ(head_) == static_cast<int>(base::Relaxed_Load(&tail_))) {
      overflow_ = true;
    } else {
      buffer_[head_] = *sample;
      head_ = Succ(head_);
      buffer_semaphore_.Signal();  // Tell we have an element.
    }
  }

  virtual void Run();

  // Pause and Resume TickSample data collection.
  void pause() { paused_ = true; }
  void resume() { paused_ = false; }

 private:
  // Waits for a signal and removes profiling data.
  bool Remove(v8::TickSample* sample) {
    buffer_semaphore_.Wait();  // Wait for an element.
    *sample = buffer_[base::Relaxed_Load(&tail_)];
    bool result = overflow_;
    base::Relaxed_Store(
        &tail_, static_cast<base::Atomic32>(Succ(base::Relaxed_Load(&tail_))));
    overflow_ = false;
    return result;
  }

  // Returns the next index in the cyclic buffer.
  int Succ(int index) { return (index + 1) % kBufferSize; }

  Isolate* isolate_;
  // Cyclic buffer for communicating profiling samples
  // between the signal handler and the worker thread.
  static const int kBufferSize = 128;
  v8::TickSample buffer_[kBufferSize];  // Buffer storage.
  int head_;  // Index to the buffer head.
  base::Atomic32 tail_;             // Index to the buffer tail.
  bool overflow_;  // Tell whether a buffer overflow has occurred.
  // Sempahore used for buffer synchronization.
  base::Semaphore buffer_semaphore_;

  // Tells whether profiler is engaged, that is, processing thread is stated.
  bool engaged_;

  // Tells whether worker thread should continue running.
  base::Atomic32 running_;

  // Tells whether we are currently recording tick samples.
  bool paused_;
};


//
// Ticker used to provide ticks to the profiler and the sliding state
// window.
//
class Ticker: public sampler::Sampler {
 public:
  Ticker(Isolate* isolate, int interval_microseconds)
      : sampler::Sampler(reinterpret_cast<v8::Isolate*>(isolate)),
        profiler_(nullptr),
        sampling_thread_(new SamplingThread(this, interval_microseconds)) {}

  ~Ticker() {
    if (IsActive()) Stop();
    delete sampling_thread_;
  }

  void SetProfiler(Profiler* profiler) {
    DCHECK_NULL(profiler_);
    profiler_ = profiler;
    IncreaseProfilingDepth();
    if (!IsActive()) Start();
    sampling_thread_->StartSynchronously();
  }

  void ClearProfiler() {
    profiler_ = nullptr;
    if (IsActive()) Stop();
    DecreaseProfilingDepth();
    sampling_thread_->Join();
  }

  void SampleStack(const v8::RegisterState& state) override {
    if (!profiler_) return;
    Isolate* isolate = reinterpret_cast<Isolate*>(this->isolate());
    TickSample sample;
    sample.Init(isolate, state, TickSample::kIncludeCEntryFrame, true);
    profiler_->Insert(&sample);
  }

 private:
  Profiler* profiler_;
  SamplingThread* sampling_thread_;
};


//
// Profiler implementation.
//
Profiler::Profiler(Isolate* isolate)
    : base::Thread(Options("v8:Profiler")),
      isolate_(isolate),
      head_(0),
      overflow_(false),
      buffer_semaphore_(0),
      engaged_(false),
      paused_(false) {
  base::Relaxed_Store(&tail_, 0);
  base::Relaxed_Store(&running_, 0);
}


void Profiler::Engage() {
  if (engaged_) return;
  engaged_ = true;

  std::vector<base::OS::SharedLibraryAddress> addresses =
      base::OS::GetSharedLibraryAddresses();
  for (size_t i = 0; i < addresses.size(); ++i) {
    LOG(isolate_,
        SharedLibraryEvent(addresses[i].library_path, addresses[i].start,
                           addresses[i].end, addresses[i].aslr_slide));
  }

  // Start thread processing the profiler buffer.
  base::Relaxed_Store(&running_, 1);
  Start();

  // Register to get ticks.
  Logger* logger = isolate_->logger();
  logger->ticker_->SetProfiler(this);

  logger->ProfilerBeginEvent();
}


void Profiler::Disengage() {
  if (!engaged_) return;

  // Stop receiving ticks.
  isolate_->logger()->ticker_->ClearProfiler();

  // Terminate the worker thread by setting running_ to false,
  // inserting a fake element in the queue and then wait for
  // the thread to terminate.
  base::Relaxed_Store(&running_, 0);
  v8::TickSample sample;
  // Reset 'paused_' flag, otherwise semaphore may not be signalled.
  resume();
  Insert(&sample);
  Join();

  LOG(isolate_, UncheckedStringEvent("profiler", "end"));
}


void Profiler::Run() {
  v8::TickSample sample;
  bool overflow = Remove(&sample);
  while (base::Relaxed_Load(&running_)) {
    LOG(isolate_, TickEvent(&sample, overflow));
    overflow = Remove(&sample);
  }
}


//
// Logger class implementation.
//

Logger::Logger(Isolate* isolate)
    : isolate_(isolate),
      ticker_(nullptr),
      profiler_(nullptr),
      log_events_(nullptr),
      is_logging_(false),
      log_(nullptr),
      perf_basic_logger_(nullptr),
      perf_jit_logger_(nullptr),
      ll_logger_(nullptr),
      jit_logger_(nullptr),
      is_initialized_(false) {}

Logger::~Logger() {
  delete log_;
}

void Logger::addCodeEventListener(CodeEventListener* listener) {
  bool result = isolate_->code_event_dispatcher()->AddListener(listener);
  USE(result);
  DCHECK(result);
}

void Logger::removeCodeEventListener(CodeEventListener* listener) {
  isolate_->code_event_dispatcher()->RemoveListener(listener);
}

void Logger::ProfilerBeginEvent() {
  if (!log_->IsEnabled()) return;
  Log::MessageBuilder msg(log_);
  msg << "profiler" << kNext << "begin" << kNext << FLAG_prof_sampling_interval;
  msg.WriteToLogFile();
}


void Logger::StringEvent(const char* name, const char* value) {
  if (FLAG_log) UncheckedStringEvent(name, value);
}


void Logger::UncheckedStringEvent(const char* name, const char* value) {
  if (!log_->IsEnabled()) return;
  Log::MessageBuilder msg(log_);
  msg << name << kNext << value;
  msg.WriteToLogFile();
}


void Logger::IntEvent(const char* name, int value) {
  if (FLAG_log) UncheckedIntEvent(name, value);
}


void Logger::IntPtrTEvent(const char* name, intptr_t value) {
  if (FLAG_log) UncheckedIntPtrTEvent(name, value);
}


void Logger::UncheckedIntEvent(const char* name, int value) {
  if (!log_->IsEnabled()) return;
  Log::MessageBuilder msg(log_);
  msg << name << kNext << value;
  msg.WriteToLogFile();
}


void Logger::UncheckedIntPtrTEvent(const char* name, intptr_t value) {
  if (!log_->IsEnabled()) return;
  Log::MessageBuilder msg(log_);
  msg << name << kNext;
  msg.Append("%" V8PRIdPTR, value);
  msg.WriteToLogFile();
}


void Logger::HandleEvent(const char* name, Object** location) {
  if (!log_->IsEnabled() || !FLAG_log_handles) return;
  Log::MessageBuilder msg(log_);
  msg << name << kNext << static_cast<void*>(location);
  msg.WriteToLogFile();
}


void Logger::ApiSecurityCheck() {
  if (!log_->IsEnabled() || !FLAG_log_api) return;
  Log::MessageBuilder msg(log_);
  msg << "api" << kNext << "check-security";
  msg.WriteToLogFile();
}

void Logger::SharedLibraryEvent(const std::string& library_path,
                                uintptr_t start, uintptr_t end,
                                intptr_t aslr_slide) {
  if (!log_->IsEnabled() || !FLAG_prof_cpp) return;
  Log::MessageBuilder msg(log_);
  msg << "shared-library" << kNext << library_path.c_str() << kNext
      << reinterpret_cast<void*>(start) << kNext << reinterpret_cast<void*>(end)
      << kNext << aslr_slide;
  msg.WriteToLogFile();
}

void Logger::CodeDeoptEvent(Code* code, DeoptKind kind, Address pc,
                            int fp_to_sp_delta) {
  if (!log_->IsEnabled()) return;
  Deoptimizer::DeoptInfo info = Deoptimizer::GetDeoptInfo(code, pc);
  Log::MessageBuilder msg(log_);
  msg << "code-deopt" << kNext << timer_.Elapsed().InMicroseconds() << kNext
      << code->CodeSize() << kNext;
  msg.AppendAddress(code->instruction_start());

  // Deoptimization position.
  std::ostringstream deopt_location;
  int inlining_id = -1;
  int script_offset = -1;
  if (info.position.IsKnown()) {
    info.position.Print(deopt_location, code);
    inlining_id = info.position.InliningId();
    script_offset = info.position.ScriptOffset();
  } else {
    deopt_location << "<unknown>";
  }
  msg << kNext << inlining_id << kNext << script_offset << kNext;
  switch (kind) {
    case kLazy:
      msg << "lazy" << kNext;
      break;
    case kSoft:
      msg << "soft" << kNext;
      break;
    case kEager:
      msg << "eager" << kNext;
      break;
  }
  msg << deopt_location.str().c_str() << kNext
      << DeoptimizeReasonToString(info.deopt_reason);
  msg.WriteToLogFile();
}


void Logger::CurrentTimeEvent() {
  if (!log_->IsEnabled()) return;
  DCHECK(FLAG_log_internal_timer_events);
  Log::MessageBuilder msg(log_);
  msg << "current-time" << kNext << timer_.Elapsed().InMicroseconds();
  msg.WriteToLogFile();
}


void Logger::TimerEvent(Logger::StartEnd se, const char* name) {
  if (!log_->IsEnabled()) return;
  Log::MessageBuilder msg(log_);
  switch (se) {
    case START:
      msg << "timer-event-start";
      break;
    case END:
      msg << "timer-event-end";
      break;
    case STAMP:
      msg << "timer-event";
  }
  msg << kNext << name << kNext << timer_.Elapsed().InMicroseconds();
  msg.WriteToLogFile();
}

// static
void Logger::EnterExternal(Isolate* isolate) {
  DCHECK(FLAG_log_internal_timer_events);
  LOG(isolate, TimerEvent(START, TimerEventExternal::name()));
  DCHECK(isolate->current_vm_state() == JS);
  isolate->set_current_vm_state(EXTERNAL);
}

// static
void Logger::LeaveExternal(Isolate* isolate) {
  DCHECK(FLAG_log_internal_timer_events);
  LOG(isolate, TimerEvent(END, TimerEventExternal::name()));
  DCHECK(isolate->current_vm_state() == EXTERNAL);
  isolate->set_current_vm_state(JS);
}

// Instantiate template methods.
#define V(TimerName, expose)                                           \
  template void TimerEventScope<TimerEvent##TimerName>::LogTimerEvent( \
      Logger::StartEnd se);
TIMER_EVENTS_LIST(V)
#undef V

void Logger::ApiNamedPropertyAccess(const char* tag, JSObject* holder,
                                    Object* property_name) {
  DCHECK(property_name->IsName());
  if (!log_->IsEnabled() || !FLAG_log_api) return;
  Log::MessageBuilder msg(log_);
  msg << "api" << kNext << tag << kNext << holder->class_name() << kNext
      << Name::cast(property_name);
  msg.WriteToLogFile();
}

void Logger::ApiIndexedPropertyAccess(const char* tag,
                                      JSObject* holder,
                                      uint32_t index) {
  if (!log_->IsEnabled() || !FLAG_log_api) return;
  Log::MessageBuilder msg(log_);
  msg << "api" << kNext << tag << kNext << holder->class_name() << kNext
      << index;
  msg.WriteToLogFile();
}


void Logger::ApiObjectAccess(const char* tag, JSObject* object) {
  if (!log_->IsEnabled() || !FLAG_log_api) return;
  Log::MessageBuilder msg(log_);
  msg << "api" << kNext << tag << kNext << object->class_name();
  msg.WriteToLogFile();
}


void Logger::ApiEntryCall(const char* name) {
  if (!log_->IsEnabled() || !FLAG_log_api) return;
  Log::MessageBuilder msg(log_);
  msg << "api" << kNext << name;
  msg.WriteToLogFile();
}


void Logger::NewEvent(const char* name, void* object, size_t size) {
  if (!log_->IsEnabled() || !FLAG_log) return;
  Log::MessageBuilder msg(log_);
  msg << "new" << kNext << name << kNext << object << kNext
      << static_cast<unsigned int>(size);
  msg.WriteToLogFile();
}


void Logger::DeleteEvent(const char* name, void* object) {
  if (!log_->IsEnabled() || !FLAG_log) return;
  Log::MessageBuilder msg(log_);
  msg << "delete" << kNext << name << kNext << object;
  msg.WriteToLogFile();
}


void Logger::CallbackEventInternal(const char* prefix, Name* name,
                                   Address entry_point) {
  if (!FLAG_log_code || !log_->IsEnabled()) return;
  Log::MessageBuilder msg(log_);
  msg << kLogEventsNames[CodeEventListener::CODE_CREATION_EVENT] << kNext
      << kLogEventsNames[CodeEventListener::CALLBACK_TAG] << kNext << -2
      << kNext << timer_.Elapsed().InMicroseconds() << kNext;
  msg.AppendAddress(entry_point);
  msg << kNext << 1 << kNext << prefix << name;
  msg.WriteToLogFile();
}


void Logger::CallbackEvent(Name* name, Address entry_point) {
  CallbackEventInternal("", name, entry_point);
}


void Logger::GetterCallbackEvent(Name* name, Address entry_point) {
  CallbackEventInternal("get ", name, entry_point);
}


void Logger::SetterCallbackEvent(Name* name, Address entry_point) {
  CallbackEventInternal("set ", name, entry_point);
}

namespace {

void AppendCodeCreateHeader(Log::MessageBuilder& msg,
                            CodeEventListener::LogEventsAndTags tag,
                            AbstractCode* code, base::ElapsedTimer* timer) {
  msg << kLogEventsNames[CodeEventListener::CODE_CREATION_EVENT]
      << Logger::kNext << kLogEventsNames[tag] << Logger::kNext << code->kind()
      << Logger::kNext << timer->Elapsed().InMicroseconds() << Logger::kNext;
  msg.AppendAddress(code->instruction_start());
  msg << Logger::kNext << code->instruction_size() << Logger::kNext;
}

}  // namespace

void Logger::CodeCreateEvent(CodeEventListener::LogEventsAndTags tag,
                             AbstractCode* code, const char* comment) {
  if (!is_logging_code_events()) return;
  if (!FLAG_log_code || !log_->IsEnabled()) return;
  Log::MessageBuilder msg(log_);
  AppendCodeCreateHeader(msg, tag, code, &timer_);
  msg << comment;
  msg.WriteToLogFile();
}

void Logger::CodeCreateEvent(CodeEventListener::LogEventsAndTags tag,
                             AbstractCode* code, Name* name) {
  if (!is_logging_code_events()) return;
  if (!FLAG_log_code || !log_->IsEnabled()) return;
  Log::MessageBuilder msg(log_);
  AppendCodeCreateHeader(msg, tag, code, &timer_);
  msg << name;
  msg.WriteToLogFile();
}

void Logger::CodeCreateEvent(CodeEventListener::LogEventsAndTags tag,
                             AbstractCode* code, SharedFunctionInfo* shared,
                             Name* name) {
  if (!is_logging_code_events()) return;
  if (!FLAG_log_code || !log_->IsEnabled()) return;
  if (code == AbstractCode::cast(
                  isolate_->builtins()->builtin(Builtins::kCompileLazy))) {
    return;
  }

  Log::MessageBuilder msg(log_);
  AppendCodeCreateHeader(msg, tag, code, &timer_);
  msg << name << kNext;
  msg.AppendAddress(shared->address());
  msg << kNext << ComputeMarker(shared, code);
  msg.WriteToLogFile();
}


// Although, it is possible to extract source and line from
// the SharedFunctionInfo object, we left it to caller
// to leave logging functions free from heap allocations.
void Logger::CodeCreateEvent(CodeEventListener::LogEventsAndTags tag,
                             AbstractCode* code, SharedFunctionInfo* shared,
                             Name* source, int line, int column) {
  if (!is_logging_code_events()) return;
  if (!FLAG_log_code || !log_->IsEnabled()) return;

  Log::MessageBuilder msg(log_);
  AppendCodeCreateHeader(msg, tag, code, &timer_);
  msg << shared->DebugName() << " " << source << ":" << line << ":" << column
      << kNext;
  msg.AppendAddress(shared->address());
  msg << kNext << ComputeMarker(shared, code);
  msg.WriteToLogFile();

  if (!FLAG_log_source_code) return;
  Object* script_object = shared->script();
  if (!script_object->IsScript()) return;
  // Make sure the script is written to the log file.
  Script* script = Script::cast(script_object);
  int script_id = script->id();
  if (logged_source_code_.find(script_id) != logged_source_code_.end()) {
    return;
  }

  // This script has not been logged yet.
  logged_source_code_.insert(script_id);
  Object* source_object = script->source();
  if (source_object->IsString()) {
    String* source_code = String::cast(source_object);
    msg << "script" << kNext << script_id << kNext;

    // Log the script name.
    if (script->name()->IsString()) {
      msg << String::cast(script->name()) << kNext;
    } else {
      msg << "<unknown>" << kNext;
    }

    // Log the source code.
    msg << source_code;
    msg.WriteToLogFile();
  }

  // We log source code information in the form:
  //
  // code-source-info <addr>,<script>,<start>,<end>,<pos>,<inline-pos>,<fns>
  //
  // where
  //   <addr> is code object address
  //   <script> is script id
  //   <start> is the starting position inside the script
  //   <end> is the end position inside the script
  //   <pos> is source position table encoded in the string,
  //      it is a sequence of C<code-offset>O<script-offset>[I<inlining-id>]
  //      where
  //        <code-offset> is the offset within the code object
  //        <script-offset> is the position within the script
  //        <inlining-id> is the offset in the <inlining> table
  //   <inlining> table is a sequence of strings of the form
  //      F<function-id>O<script-offset>[I<inlining-id>
  //      where
  //         <function-id> is an index into the <fns> function table
  //   <fns> is the function table encoded as a sequence of strings
  //      S<shared-function-info-address>
  msg << "code-source-info" << kNext
      << static_cast<void*>(code->instruction_start()) << kNext << script_id
      << kNext << shared->start_position() << kNext << shared->end_position()
      << kNext;

  SourcePositionTableIterator iterator(code->source_position_table());
  bool is_first = true;
  bool hasInlined = false;
  for (; !iterator.done(); iterator.Advance()) {
    if (is_first) {
      is_first = false;
    }
    SourcePosition pos = iterator.source_position();
    msg << "C" << iterator.code_offset() << "O" << pos.ScriptOffset();
    if (pos.isInlined()) {
      msg << "I" << pos.InliningId();
      hasInlined = true;
    }
  }
  msg << kNext;
  int maxInlinedId = -1;
  if (hasInlined) {
    PodArray<InliningPosition>* inlining_positions =
        DeoptimizationData::cast(Code::cast(code)->deoptimization_data())
            ->InliningPositions();
    for (int i = 0; i < inlining_positions->length(); i++) {
      InliningPosition inlining_pos = inlining_positions->get(i);
      msg << "F";
      if (inlining_pos.inlined_function_id != -1) {
        msg << inlining_pos.inlined_function_id;
        if (inlining_pos.inlined_function_id > maxInlinedId) {
          maxInlinedId = inlining_pos.inlined_function_id;
        }
      }
      SourcePosition pos = inlining_pos.position;
      msg << "O" << pos.ScriptOffset();
      if (pos.isInlined()) {
        msg << "I" << pos.InliningId();
      }
    }
  }
  msg << kNext;
  if (hasInlined) {
    DeoptimizationData* deopt_data =
        DeoptimizationData::cast(Code::cast(code)->deoptimization_data());

    msg << std::hex;
    for (int i = 0; i <= maxInlinedId; i++) {
      msg << "S"
          << static_cast<void*>(deopt_data->GetInlinedFunction(i)->address());
    }
    msg << std::dec;
  }
  msg.WriteToLogFile();
}

void Logger::CodeDisableOptEvent(AbstractCode* code,
                                 SharedFunctionInfo* shared) {
  if (!is_logging_code_events()) return;
  if (!FLAG_log_code || !log_->IsEnabled()) return;
  Log::MessageBuilder msg(log_);
  msg << kLogEventsNames[CodeEventListener::CODE_DISABLE_OPT_EVENT] << kNext
      << shared->DebugName() << kNext
      << GetBailoutReason(shared->disable_optimization_reason());
  msg.WriteToLogFile();
}


void Logger::CodeMovingGCEvent() {
  if (!is_logging_code_events()) return;
  if (!log_->IsEnabled() || !FLAG_ll_prof) return;
  base::OS::SignalCodeMovingGC();
}

void Logger::RegExpCodeCreateEvent(AbstractCode* code, String* source) {
  if (!is_logging_code_events()) return;
  if (!FLAG_log_code || !log_->IsEnabled()) return;
  Log::MessageBuilder msg(log_);
  AppendCodeCreateHeader(msg, CodeEventListener::REG_EXP_TAG, code, &timer_);
  msg << source;
  msg.WriteToLogFile();
}

void Logger::CodeMoveEvent(AbstractCode* from, Address to) {
  if (!is_logging_code_events()) return;
  MoveEventInternal(CodeEventListener::CODE_MOVE_EVENT, from->address(), to);
}

void Logger::CodeLinePosInfoRecordEvent(AbstractCode* code,
                                        ByteArray* source_position_table) {
  if (jit_logger_) {
    void* jit_handler_data = jit_logger_->StartCodePosInfoEvent();
    for (SourcePositionTableIterator iter(source_position_table); !iter.done();
         iter.Advance()) {
      if (iter.is_statement()) {
        jit_logger_->AddCodeLinePosInfoEvent(
            jit_handler_data, iter.code_offset(),
            iter.source_position().ScriptOffset(),
            JitCodeEvent::STATEMENT_POSITION);
      }
      jit_logger_->AddCodeLinePosInfoEvent(
          jit_handler_data, iter.code_offset(),
          iter.source_position().ScriptOffset(), JitCodeEvent::POSITION);
    }
    jit_logger_->EndCodePosInfoEvent(code, jit_handler_data);
  }
}

void Logger::CodeNameEvent(Address addr, int pos, const char* code_name) {
  if (code_name == nullptr) return;  // Not a code object.
  Log::MessageBuilder msg(log_);
  msg << kLogEventsNames[CodeEventListener::SNAPSHOT_CODE_NAME_EVENT] << kNext
      << pos << kNext << code_name;
  msg.WriteToLogFile();
}


void Logger::SharedFunctionInfoMoveEvent(Address from, Address to) {
  if (!is_logging_code_events()) return;
  MoveEventInternal(CodeEventListener::SHARED_FUNC_MOVE_EVENT, from, to);
}

void Logger::MoveEventInternal(CodeEventListener::LogEventsAndTags event,
                               Address from, Address to) {
  if (!FLAG_log_code || !log_->IsEnabled()) return;
  Log::MessageBuilder msg(log_);
  msg << kLogEventsNames[event] << kNext;
  msg.AppendAddress(from);
  msg << kNext;
  msg.AppendAddress(to);
  msg.WriteToLogFile();
}


void Logger::ResourceEvent(const char* name, const char* tag) {
  if (!log_->IsEnabled() || !FLAG_log) return;
  Log::MessageBuilder msg(log_);
  msg << name << kNext << tag << kNext;

  uint32_t sec, usec;
  if (base::OS::GetUserTime(&sec, &usec) != -1) {
    msg << sec << kNext << usec << kNext;
  }
  msg.Append("%.0f", V8::GetCurrentPlatform()->CurrentClockTimeMillis());
  msg.WriteToLogFile();
}


void Logger::SuspectReadEvent(Name* name, Object* obj) {
  if (!log_->IsEnabled() || !FLAG_log_suspect) return;
  Log::MessageBuilder msg(log_);
  String* class_name = obj->IsJSObject()
                       ? JSObject::cast(obj)->class_name()
                       : isolate_->heap()->empty_string();
  msg << "suspect-read" << kNext << class_name << kNext << name;
  msg.WriteToLogFile();
}


void Logger::HeapSampleBeginEvent(const char* space, const char* kind) {
  if (!log_->IsEnabled() || !FLAG_log_gc) return;
  Log::MessageBuilder msg(log_);
  // Using non-relative system time in order to be able to synchronize with
  // external memory profiling events (e.g. DOM memory size).
  msg << "heap-sample-begin" << kNext << space << kNext << kind << kNext;
  msg.Append("%.0f", V8::GetCurrentPlatform()->CurrentClockTimeMillis());
  msg.WriteToLogFile();
}


void Logger::HeapSampleEndEvent(const char* space, const char* kind) {
  if (!log_->IsEnabled() || !FLAG_log_gc) return;
  Log::MessageBuilder msg(log_);
  msg << "heap-sample-end" << kNext << space << kNext << kind;
  msg.WriteToLogFile();
}


void Logger::HeapSampleItemEvent(const char* type, int number, int bytes) {
  if (!log_->IsEnabled() || !FLAG_log_gc) return;
  Log::MessageBuilder msg(log_);
  msg << "heap-sample-item" << kNext << type << kNext << number << kNext
      << bytes;
  msg.WriteToLogFile();
}


void Logger::RuntimeCallTimerEvent() {
  RuntimeCallStats* stats = isolate_->counters()->runtime_call_stats();
  RuntimeCallCounter* counter = stats->current_counter();
  if (counter == nullptr) return;
  Log::MessageBuilder msg(log_);
  msg << "active-runtime-timer" << kNext << counter->name();
  msg.WriteToLogFile();
}

void Logger::TickEvent(v8::TickSample* sample, bool overflow) {
  if (!log_->IsEnabled() || !FLAG_prof_cpp) return;
  if (V8_UNLIKELY(FLAG_runtime_stats ==
                  v8::tracing::TracingCategoryObserver::ENABLED_BY_NATIVE)) {
    RuntimeCallTimerEvent();
  }
  Log::MessageBuilder msg(log_);
  msg << kLogEventsNames[CodeEventListener::TICK_EVENT] << kNext
      << reinterpret_cast<void*>(sample->pc) << kNext
      << timer_.Elapsed().InMicroseconds();
  if (sample->has_external_callback) {
    msg << kNext << 1 << kNext
        << reinterpret_cast<void*>(sample->external_callback_entry);
  } else {
    msg << kNext << 0 << kNext << reinterpret_cast<void*>(sample->tos);
  }
  msg << kNext << static_cast<int>(sample->state);
  if (overflow) msg << kNext << "overflow";
  for (unsigned i = 0; i < sample->frames_count; ++i) {
    msg << kNext << reinterpret_cast<void*>(sample->stack[i]);
  }
  msg.WriteToLogFile();
}

void Logger::ICEvent(const char* type, bool keyed, Map* map, Object* key,
                     char old_state, char new_state, const char* modifier,
                     const char* slow_stub_reason) {
  if (!log_->IsEnabled() || !FLAG_trace_ic) return;
  Log::MessageBuilder msg(log_);
  if (keyed) msg << "Keyed";
  msg << type << kNext;
  int line;
  int column;
  Address pc = isolate_->GetAbstractPC(&line, &column);
  msg.AppendAddress(pc);
  msg << kNext << line << kNext << column << kNext << old_state << kNext
      << new_state << kNext << reinterpret_cast<void*>(map) << kNext;
  if (key->IsSmi()) {
    msg << Smi::ToInt(key);
  } else if (key->IsNumber()) {
    msg << key->Number();
  } else if (key->IsName()) {
    msg << Name::cast(key);
  }
  msg << kNext << modifier << kNext;
  if (slow_stub_reason != nullptr) {
    msg << slow_stub_reason;
  }
  msg.WriteToLogFile();
}

void Logger::StopProfiler() {
  if (!log_->IsEnabled()) return;
  if (profiler_ != nullptr) {
    profiler_->pause();
    is_logging_ = false;
    removeCodeEventListener(this);
  }
}

// This function can be called when Log's mutex is acquired,
// either from main or Profiler's thread.
void Logger::LogFailure() {
  StopProfiler();
}

static void AddFunctionAndCode(SharedFunctionInfo* sfi,
                               AbstractCode* code_object,
                               Handle<SharedFunctionInfo>* sfis,
                               Handle<AbstractCode>* code_objects, int offset) {
  if (sfis != nullptr) {
    sfis[offset] = Handle<SharedFunctionInfo>(sfi);
  }
  if (code_objects != nullptr) {
    code_objects[offset] = Handle<AbstractCode>(code_object);
  }
}

static int EnumerateCompiledFunctions(Heap* heap,
                                      Handle<SharedFunctionInfo>* sfis,
                                      Handle<AbstractCode>* code_objects) {
  HeapIterator iterator(heap);
  DisallowHeapAllocation no_gc;
  int compiled_funcs_count = 0;

  // Iterate the heap to find shared function info objects and record
  // the unoptimized code for them.
  for (HeapObject* obj = iterator.next(); obj != nullptr;
       obj = iterator.next()) {
    if (obj->IsSharedFunctionInfo()) {
      SharedFunctionInfo* sfi = SharedFunctionInfo::cast(obj);
      if (sfi->is_compiled() &&
          (!sfi->script()->IsScript() ||
           Script::cast(sfi->script())->HasValidSource())) {
        // In some cases, an SFI might have (and have executing!) both bytecode
        // and baseline code, so check for both and add them both if needed.
        if (sfi->HasBytecodeArray()) {
          AddFunctionAndCode(sfi, AbstractCode::cast(sfi->bytecode_array()),
                             sfis, code_objects, compiled_funcs_count);
          ++compiled_funcs_count;
        }

        if (!sfi->IsInterpreted()) {
          AddFunctionAndCode(sfi, AbstractCode::cast(sfi->code()), sfis,
                             code_objects, compiled_funcs_count);
          ++compiled_funcs_count;
        }
      }
    } else if (obj->IsJSFunction()) {
      // Given that we no longer iterate over all optimized JSFunctions, we need
      // to take care of this here.
      JSFunction* function = JSFunction::cast(obj);
      SharedFunctionInfo* sfi = SharedFunctionInfo::cast(function->shared());
      Object* maybe_script = sfi->script();
      if (maybe_script->IsScript() &&
          !Script::cast(maybe_script)->HasValidSource()) {
        continue;
      }
      // TODO(jarin) This leaves out deoptimized code that might still be on the
      // stack. Also note that we will not log optimized code objects that are
      // only on a type feedback vector. We should make this mroe precise.
      if (function->IsOptimized()) {
        AddFunctionAndCode(sfi, AbstractCode::cast(function->code()), sfis,
                           code_objects, compiled_funcs_count);
        ++compiled_funcs_count;
      }
    }
  }
  return compiled_funcs_count;
}


void Logger::LogCodeObject(Object* object) {
  AbstractCode* code_object = AbstractCode::cast(object);
  CodeEventListener::LogEventsAndTags tag = CodeEventListener::STUB_TAG;
  const char* description = "Unknown code from the snapshot";
  switch (code_object->kind()) {
    case AbstractCode::INTERPRETED_FUNCTION:
    case AbstractCode::OPTIMIZED_FUNCTION:
      return;  // We log this later using LogCompiledFunctions.
    case AbstractCode::BYTECODE_HANDLER:
      return;  // We log it later by walking the dispatch table.
    case AbstractCode::STUB:
      description =
          CodeStub::MajorName(CodeStub::GetMajorKey(code_object->GetCode()));
      if (description == nullptr) description = "A stub from the snapshot";
      tag = CodeEventListener::STUB_TAG;
      break;
    case AbstractCode::REGEXP:
      description = "Regular expression code";
      tag = CodeEventListener::REG_EXP_TAG;
      break;
    case AbstractCode::BUILTIN:
      description =
          isolate_->builtins()->name(code_object->GetCode()->builtin_index());
      tag = CodeEventListener::BUILTIN_TAG;
      break;
    case AbstractCode::WASM_FUNCTION:
      description = "A Wasm function";
      tag = CodeEventListener::STUB_TAG;
      break;
    case AbstractCode::JS_TO_WASM_FUNCTION:
      description = "A JavaScript to Wasm adapter";
      tag = CodeEventListener::STUB_TAG;
      break;
    case AbstractCode::WASM_TO_JS_FUNCTION:
      description = "A Wasm to JavaScript adapter";
      tag = CodeEventListener::STUB_TAG;
      break;
    case AbstractCode::WASM_INTERPRETER_ENTRY:
      description = "A Wasm to Interpreter adapter";
      tag = CodeEventListener::STUB_TAG;
      break;
    case AbstractCode::C_WASM_ENTRY:
      description = "A C to Wasm entry stub";
      tag = CodeEventListener::STUB_TAG;
      break;
    case AbstractCode::NUMBER_OF_KINDS:
      UNIMPLEMENTED();
  }
  PROFILE(isolate_, CodeCreateEvent(tag, code_object, description));
}


void Logger::LogCodeObjects() {
  Heap* heap = isolate_->heap();
  HeapIterator iterator(heap);
  DisallowHeapAllocation no_gc;
  for (HeapObject* obj = iterator.next(); obj != nullptr;
       obj = iterator.next()) {
    if (obj->IsCode()) LogCodeObject(obj);
    if (obj->IsBytecodeArray()) LogCodeObject(obj);
  }
}

void Logger::LogBytecodeHandlers() {
  const interpreter::OperandScale kOperandScales[] = {
#define VALUE(Name, _) interpreter::OperandScale::k##Name,
      OPERAND_SCALE_LIST(VALUE)
#undef VALUE
  };

  const int last_index = static_cast<int>(interpreter::Bytecode::kLast);
  interpreter::Interpreter* interpreter = isolate_->interpreter();
  for (auto operand_scale : kOperandScales) {
    for (int index = 0; index <= last_index; ++index) {
      interpreter::Bytecode bytecode = interpreter::Bytecodes::FromByte(index);
      if (interpreter::Bytecodes::BytecodeHasHandler(bytecode, operand_scale)) {
        Code* code = interpreter->GetBytecodeHandler(bytecode, operand_scale);
        std::string bytecode_name =
            interpreter::Bytecodes::ToString(bytecode, operand_scale);
        PROFILE(isolate_, CodeCreateEvent(
                              CodeEventListener::BYTECODE_HANDLER_TAG,
                              AbstractCode::cast(code), bytecode_name.c_str()));
      }
    }
  }
}

void Logger::LogExistingFunction(Handle<SharedFunctionInfo> shared,
                                 Handle<AbstractCode> code) {
  if (shared->script()->IsScript()) {
    Handle<Script> script(Script::cast(shared->script()));
    int line_num = Script::GetLineNumber(script, shared->start_position()) + 1;
    int column_num =
        Script::GetColumnNumber(script, shared->start_position()) + 1;
    if (script->name()->IsString()) {
      Handle<String> script_name(String::cast(script->name()));
      if (line_num > 0) {
        PROFILE(isolate_,
                CodeCreateEvent(
                    Logger::ToNativeByScript(
                        CodeEventListener::LAZY_COMPILE_TAG, *script),
                    *code, *shared, *script_name, line_num, column_num));
      } else {
        // Can't distinguish eval and script here, so always use Script.
        PROFILE(isolate_,
                CodeCreateEvent(Logger::ToNativeByScript(
                                    CodeEventListener::SCRIPT_TAG, *script),
                                *code, *shared, *script_name));
      }
    } else {
      PROFILE(isolate_,
              CodeCreateEvent(Logger::ToNativeByScript(
                                  CodeEventListener::LAZY_COMPILE_TAG, *script),
                              *code, *shared, isolate_->heap()->empty_string(),
                              line_num, column_num));
    }
  } else if (shared->IsApiFunction()) {
    // API function.
    FunctionTemplateInfo* fun_data = shared->get_api_func_data();
    Object* raw_call_data = fun_data->call_code();
    if (!raw_call_data->IsUndefined(isolate_)) {
      CallHandlerInfo* call_data = CallHandlerInfo::cast(raw_call_data);
      Object* callback_obj = call_data->callback();
      Address entry_point = v8::ToCData<Address>(callback_obj);
#if USES_FUNCTION_DESCRIPTORS
      entry_point = *FUNCTION_ENTRYPOINT_ADDRESS(entry_point);
#endif
      PROFILE(isolate_, CallbackEvent(shared->DebugName(), entry_point));
    }
  } else {
    PROFILE(isolate_,
            CodeCreateEvent(CodeEventListener::LAZY_COMPILE_TAG, *code, *shared,
                            isolate_->heap()->empty_string()));
  }
}


void Logger::LogCompiledFunctions() {
  Heap* heap = isolate_->heap();
  HandleScope scope(isolate_);
  const int compiled_funcs_count =
      EnumerateCompiledFunctions(heap, nullptr, nullptr);
  ScopedVector< Handle<SharedFunctionInfo> > sfis(compiled_funcs_count);
  ScopedVector<Handle<AbstractCode> > code_objects(compiled_funcs_count);
  EnumerateCompiledFunctions(heap, sfis.start(), code_objects.start());

  // During iteration, there can be heap allocation due to
  // GetScriptLineNumber call.
  for (int i = 0; i < compiled_funcs_count; ++i) {
    if (code_objects[i].is_identical_to(BUILTIN_CODE(isolate_, CompileLazy)))
      continue;
    LogExistingFunction(sfis[i], code_objects[i]);
  }
}


void Logger::LogAccessorCallbacks() {
  Heap* heap = isolate_->heap();
  HeapIterator iterator(heap);
  DisallowHeapAllocation no_gc;
  for (HeapObject* obj = iterator.next(); obj != nullptr;
       obj = iterator.next()) {
    if (!obj->IsAccessorInfo()) continue;
    AccessorInfo* ai = AccessorInfo::cast(obj);
    if (!ai->name()->IsName()) continue;
    Address getter_entry = v8::ToCData<Address>(ai->getter());
    Name* name = Name::cast(ai->name());
    if (getter_entry != 0) {
#if USES_FUNCTION_DESCRIPTORS
      getter_entry = *FUNCTION_ENTRYPOINT_ADDRESS(getter_entry);
#endif
      PROFILE(isolate_, GetterCallbackEvent(name, getter_entry));
    }
    Address setter_entry = v8::ToCData<Address>(ai->setter());
    if (setter_entry != 0) {
#if USES_FUNCTION_DESCRIPTORS
      setter_entry = *FUNCTION_ENTRYPOINT_ADDRESS(setter_entry);
#endif
      PROFILE(isolate_, SetterCallbackEvent(name, setter_entry));
    }
  }
}


static void AddIsolateIdIfNeeded(std::ostream& os,  // NOLINT
                                 Isolate* isolate) {
  if (FLAG_logfile_per_isolate) os << "isolate-" << isolate << "-";
}


static void PrepareLogFileName(std::ostream& os,  // NOLINT
                               Isolate* isolate, const char* file_name) {
  int dir_separator_count = 0;
  for (const char* p = file_name; *p; p++) {
    if (base::OS::isDirectorySeparator(*p)) dir_separator_count++;
  }

  for (const char* p = file_name; *p; p++) {
    if (dir_separator_count == 0) {
      AddIsolateIdIfNeeded(os, isolate);
      dir_separator_count--;
    }
    if (*p == '%') {
      p++;
      switch (*p) {
        case '\0':
          // If there's a % at the end of the string we back up
          // one character so we can escape the loop properly.
          p--;
          break;
        case 'p':
          os << base::OS::GetCurrentProcessId();
          break;
        case 't':
          // %t expands to the current time in milliseconds.
          os << static_cast<int64_t>(
              V8::GetCurrentPlatform()->CurrentClockTimeMillis());
          break;
        case '%':
          // %% expands (contracts really) to %.
          os << '%';
          break;
        default:
          // All other %'s expand to themselves.
          os << '%' << *p;
          break;
      }
    } else {
      if (base::OS::isDirectorySeparator(*p)) dir_separator_count--;
      os << *p;
    }
  }
}


bool Logger::SetUp(Isolate* isolate) {
  // Tests and EnsureInitialize() can call this twice in a row. It's harmless.
  if (is_initialized_) return true;
  is_initialized_ = true;

  std::ostringstream log_file_name;
  std::ostringstream source_log_file_name;
  PrepareLogFileName(log_file_name, isolate, FLAG_logfile);
  log_ = new Log(this, log_file_name.str().c_str());

  if (FLAG_perf_basic_prof) {
    perf_basic_logger_ = new PerfBasicLogger();
    addCodeEventListener(perf_basic_logger_);
  }

  if (FLAG_perf_prof) {
    perf_jit_logger_ = new PerfJitLogger();
    addCodeEventListener(perf_jit_logger_);
  }

  if (FLAG_ll_prof) {
    ll_logger_ = new LowLevelLogger(log_file_name.str().c_str());
    addCodeEventListener(ll_logger_);
  }

  ticker_ = new Ticker(isolate, FLAG_prof_sampling_interval);

  if (Log::InitLogAtStart()) {
    is_logging_ = true;
  }

  timer_.Start();

  if (FLAG_prof_cpp) {
    profiler_ = new Profiler(isolate);
    is_logging_ = true;
    profiler_->Engage();
  }

  profiler_listener_.reset();

  if (is_logging_) {
    addCodeEventListener(this);
  }

  return true;
}


void Logger::SetCodeEventHandler(uint32_t options,
                                 JitCodeEventHandler event_handler) {
  if (jit_logger_) {
      removeCodeEventListener(jit_logger_);
      delete jit_logger_;
      jit_logger_ = nullptr;
  }

  if (event_handler) {
    jit_logger_ = new JitLogger(event_handler);
    addCodeEventListener(jit_logger_);
    if (options & kJitCodeEventEnumExisting) {
      HandleScope scope(isolate_);
      LogCodeObjects();
      LogCompiledFunctions();
    }
  }
}

void Logger::SetUpProfilerListener() {
  if (!is_initialized_) return;
  if (profiler_listener_.get() == nullptr) {
    profiler_listener_.reset(new ProfilerListener(isolate_));
  }
  addCodeEventListener(profiler_listener_.get());
}

void Logger::TearDownProfilerListener() {
  if (profiler_listener_->HasObservers()) return;
  removeCodeEventListener(profiler_listener_.get());
}

sampler::Sampler* Logger::sampler() {
  return ticker_;
}


FILE* Logger::TearDown() {
  if (!is_initialized_) return nullptr;
  is_initialized_ = false;

  // Stop the profiler before closing the file.
  if (profiler_ != nullptr) {
    profiler_->Disengage();
    delete profiler_;
    profiler_ = nullptr;
  }

  delete ticker_;
  ticker_ = nullptr;

  if (perf_basic_logger_) {
    removeCodeEventListener(perf_basic_logger_);
    delete perf_basic_logger_;
    perf_basic_logger_ = nullptr;
  }

  if (perf_jit_logger_) {
    removeCodeEventListener(perf_jit_logger_);
    delete perf_jit_logger_;
    perf_jit_logger_ = nullptr;
  }

  if (ll_logger_) {
    removeCodeEventListener(ll_logger_);
    delete ll_logger_;
    ll_logger_ = nullptr;
  }

  if (jit_logger_) {
    removeCodeEventListener(jit_logger_);
    delete jit_logger_;
    jit_logger_ = nullptr;
  }

  if (profiler_listener_.get() != nullptr) {
    removeCodeEventListener(profiler_listener_.get());
  }

  return log_->Close();
}

}  // namespace internal
}  // namespace v8
