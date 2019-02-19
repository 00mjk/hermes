/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the LICENSE
 * file in the root directory of this source tree.
 */
#include "hermes.h"

// Helper macro for deciding if the tracer should be used.
#if defined(HERMESVM_API_TRACE) && !defined(HERMESVM_LEAN)
#define API_TRACE 1
#else
#define API_TRACE 0
#endif

#if LLVM_PTR_SIZE != 8
// Only have JSI be on the stack for builds that are not 64-bit.
#define HERMESJSI_ON_STACK
#endif

#if API_TRACE
#include "SynthTrace.h"
#endif

#include "hermes/BCGen/HBC/BytecodeDataProvider.h"
#include "hermes/BCGen/HBC/BytecodeProviderFromSrc.h"
#include "hermes/DebuggerAPI.h"
#include "hermes/Instrumentation/PerfMarkers.h"
#include "hermes/Platform/Logging.h"
#include "hermes/Public/RuntimeConfig.h"
#include "hermes/Support/Algorithms.h"
#include "hermes/Support/UTF8.h"
#include "hermes/VM/CallResult.h"
#include "hermes/VM/Debugger/Debugger.h"
#include "hermes/VM/GC.h"
#include "hermes/VM/HostModel.h"
#include "hermes/VM/JSArray.h"
#include "hermes/VM/JSArrayBuffer.h"
#include "hermes/VM/JSError.h"
#include "hermes/VM/JSLib.h"
#include "hermes/VM/JSLib/RuntimeCommonStorage.h"
#include "hermes/VM/Operations.h"
#include "hermes/VM/Profiler/SamplingProfiler.h"
#include "hermes/VM/Runtime.h"
#include "hermes/VM/StringPrimitive.h"
#include "hermes/VM/StringView.h"

#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/SHA1.h"

#include <atomic>
#include <limits>
#include <list>
#include <mutex>
#include <system_error>

#ifdef HERMESJSI_ON_STACK
#include <future>
#include <thread>
#endif

#include <jsi/instrumentation.h>

#ifdef HERMESVM_LLVM_PROFILE_DUMP
extern "C" {
int __llvm_profile_dump(void);
}
#endif

namespace vm = hermes::vm;
namespace hbc = hermes::hbc;
using ::hermes::hermesLog;

namespace facebook {
namespace hermes {
namespace detail {

static void (*sApiFatalHandler)(const std::string &) = nullptr;
/// Handler called by HermesVM to report unrecoverable errors.
/// This is a forward declaration to prevent a compiler warning.
void hermesFatalErrorHandler(
    void *user_data,
    const std::string &reason,
    bool gen_crash_diag);

void hermesFatalErrorHandler(
    void * /*user_data*/,
    const std::string &reason,
    bool /*gen_crash_diag*/) {
  // Actually crash and let breakpad handle the reporting.
  if (sApiFatalHandler) {
    sApiFatalHandler(reason);
  } else {
    *((volatile int *)nullptr) = 42;
  }
}

} // namespace detail

namespace {

// Max size of the runtime's register stack.
// The runtime register stack needs to be small enough to be allocated on the
// native thread stack in Android (1MiB) and on MacOS's thread stack (512 KiB)
// Calculated by: (thread stack size - size of runtime -
// 8 memory pages for other stuff in the thread)
static constexpr unsigned kMaxNumRegisters =
    (512 * 1024 - sizeof(::hermes::vm::Runtime) - 4096 * 8) /
    sizeof(::hermes::vm::PinnedHermesValue);

// The minimum code size in bytes before enabling lazy compilation.
// Lazy compilation has significant per-module overhead, and is best applied
// to large bundles with a lot of unused code. Eager compilation is more
// efficient when compiling many small bundles with little unused code, such as
// when the API user loads smaller chunks of JS code on demand.
static constexpr unsigned kMinimumLazySize = 1 << 16;

void raw_ostream_append(llvm::raw_ostream &os) {}

template <typename Arg0, typename... Args>
void raw_ostream_append(llvm::raw_ostream &os, Arg0 &&arg0, Args &&... args) {
  os << arg0;
  raw_ostream_append(os, args...);
}

template <typename... Args>
jsi::JSError makeJSError(jsi::Runtime &rt, Args &&... args) {
  std::string s;
  llvm::raw_string_ostream os(s);
  raw_ostream_append(os, std::forward<Args>(args)...);
  return jsi::JSError(rt, os.str());
}

class HermesRuntimeImpl;

struct Lock {
  void lock(const HermesRuntimeImpl &) const {
    mutex_.lock();
  }

  void unlock(const HermesRuntimeImpl &) const {
    mutex_.unlock();
  }

 private:
  mutable std::recursive_mutex mutex_;
};

/// HermesVM uses the LLVM fatal error handle to report fatal errors. This
/// wrapper helps us install the handler at construction time, before any
/// HermesVM code has been invoked.
class InstallHermesFatalErrorHandler {
 public:
  InstallHermesFatalErrorHandler() {
    // The LLVM fatal error handler can only be installed once. Use a Meyer's
    // singleton to guarantee it - the static "dummy" is guaranteed by the
    // compiler to be initialized no more than once.
    static int dummy = ([]() {
      llvm::install_fatal_error_handler(detail::hermesFatalErrorHandler);
      return 0;
    })();
    (void)dummy;
  }
};

#ifdef HERMESJSI_ON_STACK
// Minidumps include stack memory, not heap memory.  If we want to be
// able to inspect the Runtime object in a minidump, we can do that by
// arranging for it to be allocated on a stack.  No existing stack is
// a good candidate, so we achieve this by creating a thread just to
// hold the Runtime.

class StackRuntime {
 public:
  StackRuntime(const vm::RuntimeConfig &runtimeConfig)
      : thread_(runtimeMemoryThread, this, runtimeConfig.getGCConfig()) {
    startup_.get_future().wait();
    runtime_->emplace(
        provider_.get(),
        runtimeConfig.rebuild()
            .withRegisterStack(registerStack_)
            .withMaxNumRegisters(kMaxNumRegisters)
            .build());
  }

  ~StackRuntime() {
    // We can't shut down the Runtime on the captive thread, because
    // it might need to make JNI calls to clean up HostObjects.  So we
    // delete it from here, which is going to be on a thread
    // registered with the JVM.
    runtime_->reset();
    shutdown_.set_value();
    thread_.join();
    runtime_ = nullptr;
    registerStack_ = nullptr;
  }

  ::hermes::vm::Runtime &getRuntime() {
    return **runtime_;
  }

 private:
  static void runtimeMemoryThread(StackRuntime *stack, vm::GCConfig config) {
#if defined(__APPLE__)
    // Capture the thread name in case if something was already set, so that we
    // can restore it later when we're potentially returning the thread back
    // to some pool.
    char buf[256];
    int getNameSuccess = pthread_getname_np(pthread_self(), buf, sizeof(buf));

    pthread_setname_np("hermes-runtime-memorythread");
#endif

    llvm::Optional<::hermes::vm::StackRuntime> rt;
    typename std::aligned_storage<
        sizeof(::hermes::vm::PinnedHermesValue),
        alignof(::hermes::vm::PinnedHermesValue)>::type
        registerStack[kMaxNumRegisters];

    stack->provider_ = vm::StorageProvider::defaultProvider(
        vm::GC::Size{config.getMinHeapSize(), config.getMaxHeapSize()}
            .storageFootprint());
    stack->runtime_ = &rt;
    stack->registerStack_ =
        reinterpret_cast<::hermes::vm::PinnedHermesValue *>(registerStack);
    stack->startup_.set_value();
    stack->shutdown_.get_future().wait();
    assert(!rt.hasValue() && "Runtime was not torn down before thread");

#if defined(__APPLE__)
    if (!getNameSuccess) {
      pthread_setname_np(buf);
    }
#endif
  }

  // The order here matters.
  // * Set up the promises
  // * Initialize various pointers to null
  // * Start the thread which uses them
  // * Initialize provider_, runtime_, and registerStack_ from that thread
  std::promise<void> startup_;
  std::promise<void> shutdown_;
  std::unique_ptr<::hermes::vm::StorageProvider> provider_;
  llvm::Optional<::hermes::vm::StackRuntime> *runtime_{nullptr};
  ::hermes::vm::PinnedHermesValue *registerStack_{nullptr};
  std::thread thread_;
};
#endif

class HermesRuntimeImpl
#if !API_TRACE
    final
#endif
    : public HermesRuntime,
      private InstallHermesFatalErrorHandler,
      private jsi::Instrumentation {
 public:
  static constexpr int64_t kSentinelNativeValue = 0x6ef71fe1;

#if API_TRACE
 protected:
#endif
  HermesRuntimeImpl(const vm::RuntimeConfig &runtimeConfig)
      :
#ifdef HERMESJSI_ON_STACK
        stackRuntime_(runtimeConfig),
        runtime_(stackRuntime_.getRuntime())
#else
        rt_(::hermes::vm::Runtime::create(
            runtimeConfig.rebuild()
                .withRegisterStack(nullptr)
                .withMaxNumRegisters(kMaxNumRegisters)
                .build())),
        runtime_(*rt_)
#endif
  {
    runtime_.addCustomRootsFunction([this](
                                        vm::GC *, vm::SlotAcceptor &acceptor) {
      for (auto it = hermesValues_->begin(); it != hermesValues_->end();) {
        if (it->get() == 0) {
          it = hermesValues_->erase(it);
        } else {
          acceptor.accept(const_cast<vm::PinnedHermesValue &>(it->phv));
          ++it;
        }
      }
      for (auto it = weakHermesValues_->begin();
           it != weakHermesValues_->end();) {
        if (it->get() == 0) {
          it = weakHermesValues_->erase(it);
        } else {
          acceptor.accept(const_cast<vm::WeakRef<vm::HermesValue> &>(it->wr));
          ++it;
        }
      }
    });
  }

 public:
  ~HermesRuntimeImpl() {
#ifdef HERMES_ENABLE_DEBUGGER
    // Deallocate the debugger so it frees any HermesPointerValues it may hold.
    // This must be done before we check hermesValues_ below.
    debugger_.reset();
#endif
  }

#ifdef HERMES_ENABLE_DEBUGGER
  // This should only be called once by the factory.
  void setDebugger(std::unique_ptr<debugger::Debugger> d) {
    debugger_ = std::move(d);
  }
#endif

  struct CountedPointerValue : PointerValue {
    CountedPointerValue() : refCount(1) {}

    void invalidate() override {
      dec();
    }

    void inc() {
      auto oldCount = refCount.fetch_add(1, std::memory_order_relaxed);
      assert(oldCount + 1 != 0 && "Ref count overflow");
      (void)oldCount;
    }

    void dec() {
      auto oldCount = refCount.fetch_sub(1, std::memory_order_relaxed);
      assert(oldCount > 0 && "Ref count underflow");
      (void)oldCount;
    }

    uint32_t get() const {
      return refCount.load(std::memory_order_relaxed);
    }

    std::atomic<uint32_t> refCount;
  };

  struct HermesPointerValue final : CountedPointerValue {
    HermesPointerValue(vm::HermesValue hv) : phv(hv) {}

    // This should only ever be modified by the GC.  We const_cast the
    // reference before passing it to the GC.
    const vm::PinnedHermesValue phv;
  };

  struct WeakRefPointerValue final : CountedPointerValue {
    WeakRefPointerValue(vm::WeakRef<vm::HermesValue> _wr) : wr(_wr) {}

    // This should only ever be modified by the GC.  We const_cast the
    // reference before passing it to the GC.
    const vm::WeakRef<vm::HermesValue> wr;
  };

  HermesPointerValue *clone(const Runtime::PointerValue *pv) {
    if (!pv) {
      return nullptr;
    }
    // These are only ever allocated by us, so we can remove their constness
    auto result = static_cast<HermesPointerValue *>(
        const_cast<Runtime::PointerValue *>(pv));
    result->inc();
    return result;
  }

  template <typename T>
  T add(::hermes::vm::HermesValue hv) {
    static_assert(
        std::is_base_of<jsi::Pointer, T>::value, "this type cannot be added");
    hermesValues_->emplace_front(hv);
    return make<T>(&(hermesValues_->front()));
  }

  jsi::WeakObject addWeak(::hermes::vm::WeakRef<vm::HermesValue> wr) {
    weakHermesValues_->emplace_front(wr);
    return make<jsi::WeakObject>(&(weakHermesValues_->front()));
  }

  // overriden from jsi::Instrumentation
  std::string getRecordedGCStats() override {
    std::string s;
    llvm::raw_string_ostream os(s);

    runtime_.getHeap().printAllCollectedStats(os);
    return os.str();
  }

  void ttiReached() override {
    runtime_.ttiReached();
#ifdef HERMESVM_LLVM_PROFILE_DUMP
    __llvm_profile_dump();
    throw jsi::JSINativeException("TTI reached; profiling done");
#endif
  }

  // Overridden from jsi::Instrumentation
  jsi::Value getHeapInfo(bool includeExpensive) override {
    vm::GCBase::HeapInfo info;
    if (includeExpensive) {
      runtime_.getHeap().getHeapInfoWithMallocSize(info);
    } else {
      runtime_.getHeap().getHeapInfo(info);
    }
#ifndef NDEBUG
    vm::GCBase::DebugHeapInfo debugInfo;
    runtime_.getHeap().getDebugHeapInfo(debugInfo);
#endif

    auto jsInfo = createObject();

#define BRIDGE_INFO(TYPE, HOLDER, NAME) \
  jsInfo.setProperty(*this, "hermes_" #NAME, static_cast<TYPE>(HOLDER.NAME))

    BRIDGE_INFO(int, info, numCollections);
    BRIDGE_INFO(int, info, totalAllocatedBytes);
    BRIDGE_INFO(int, info, allocatedBytes);
    BRIDGE_INFO(int, info, heapSize);
    BRIDGE_INFO(int, info, va);
    if (includeExpensive) {
      BRIDGE_INFO(int, info, mallocSizeEstimate);
    }

#ifndef NDEBUG
    BRIDGE_INFO(int, debugInfo, numAllocatedObjects);
    BRIDGE_INFO(int, debugInfo, numReachableObjects);
    BRIDGE_INFO(int, debugInfo, numCollectedObjects);
    BRIDGE_INFO(int, debugInfo, numFinalizedObjects);
    BRIDGE_INFO(int, debugInfo, numMarkedSymbols);
    BRIDGE_INFO(int, debugInfo, numHiddenClasses);
    BRIDGE_INFO(int, debugInfo, numLeafHiddenClasses);
#endif

#undef BRIDGE_INFO

#define BRIDGE_GEN_INFO(NAME, STAT_EXPR, FACTOR)                      \
  jsInfo.setProperty(                                                 \
      *this, "hermes_full_" #NAME, info.fullStats.STAT_EXPR *FACTOR); \
  jsInfo.setProperty(                                                 \
      *this, "hermes_yg_" #NAME, info.youngGenStats.STAT_EXPR *FACTOR)

    BRIDGE_GEN_INFO(numCollections, numCollections, 1.0);
    // Times are converted from seconds to milliseconds for the logging pipeline
    // ...
    BRIDGE_GEN_INFO(gcTime, gcWallTime.sum(), 1000);
    BRIDGE_GEN_INFO(maxPause, gcWallTime.max(), 1000);
    BRIDGE_GEN_INFO(gcCPUTime, gcCPUTime.sum(), 1000);
    BRIDGE_GEN_INFO(gcMaxCPUPause, gcCPUTime.max(), 1000);
    // ... and since this is square seconds, we must square the 1000 too.
    BRIDGE_GEN_INFO(gcTimeSquares, gcWallTime.sumOfSquares(), 1000 * 1000);
    BRIDGE_GEN_INFO(gcCPUTimeSquares, gcCPUTime.sumOfSquares(), 1000 * 1000);

#undef BRIDGE_GEN_INFO

    return std::move(jsInfo);
  }

  // Overridden from jsi::Instrumentation
  void collectGarbage() override {
    runtime_.getHeap().collect();
  }

  // Overridden from jsi::Instrumentation
  bool createSnapshotToFile(const std::string &path, bool compact = true)
      override {
    vm::GC &gc = runtime_.getHeap();
    gc.collect();
    return gc.createSnapshotToFile(path, compact);
  }

  // Overridden from jsi::Instrumentation
  void writeBridgeTrafficTraceToFile(
      const std::string &fileName) const override {
    throw std::logic_error(
        "Cannot write the bridge traffic trace out if Hermes wasn't built with "
        "@fbsource//xplat/mode/hermes/trace");
  }

  // Overridden from jsi::Instrumentation
  void writeBasicBlockProfileTraceToFile(
      const std::string &fileName) const override {
#ifdef HERMESVM_PROFILER_BB
    std::error_code ec;
    llvm::raw_fd_ostream os(fileName.c_str(), ec, llvm::sys::fs::F_Text);
    if (ec) {
      throw std::system_error(ec);
    }
    runtime_.dumpBasicBlockProfileTrace(os);
#else
    throw std::logic_error(
        "Cannot write the basic block profile trace out if Hermes wasn't built with "
        "hermes.profiler=BB");
#endif
  }

  void registerForProfiling() override {
    ::hermes::vm::SamplingProfiler::getInstance()->registerRuntime(&runtime_);
  }

  void unregisterForProfiling() override {
    ::hermes::vm::SamplingProfiler::getInstance()->unregisterRuntime(&runtime_);
  }

  // Overridden from jsi::Instrumentation
  void dumpProfilerSymbolsToFile(const std::string &fileName) const override {
#ifdef HERMESVM_PROFILER_EXTERN
    dumpProfilerSymbolMap(&runtime_, fileName);
#else
    throw std::logic_error(
        "Cannot dump profiler symbols out if Hermes wasn't built with "
        "hermes.profiler=EXTERN");
#endif
  }

  // These are all methods which do pointer type gymnastics and should
  // mostly inline and optimize away.

  static const ::hermes::vm::PinnedHermesValue &phv(
      const jsi::Pointer &pointer) {
    assert(
        dynamic_cast<const HermesPointerValue *>(getPointerValue(pointer)) &&
        "Pointer does not contain a HermesPointerValue");
    return static_cast<const HermesPointerValue *>(getPointerValue(pointer))
        ->phv;
  }

  static const ::hermes::vm::PinnedHermesValue &phv(const jsi::Value &value) {
    assert(
        dynamic_cast<const HermesPointerValue *>(getPointerValue(value)) &&
        "Pointer does not contain a HermesPointerValue");
    return static_cast<const HermesPointerValue *>(getPointerValue(value))->phv;
  }

  static ::hermes::vm::Handle<::hermes::vm::HermesValue> stringHandle(
      const jsi::String &str) {
    return ::hermes::vm::Handle<::hermes::vm::HermesValue>::vmcast(&phv(str));
  }

  static ::hermes::vm::Handle<::hermes::vm::JSObject> handle(
      const jsi::Object &obj) {
    return ::hermes::vm::Handle<::hermes::vm::JSObject>::vmcast(&phv(obj));
  }

  static ::hermes::vm::Handle<::hermes::vm::JSArray> arrayHandle(
      const jsi::Array &arr) {
    return ::hermes::vm::Handle<::hermes::vm::JSArray>::vmcast(&phv(arr));
  }

  static ::hermes::vm::Handle<::hermes::vm::JSArrayBuffer> arrayBufferHandle(
      const jsi::ArrayBuffer &arr) {
    return ::hermes::vm::Handle<::hermes::vm::JSArrayBuffer>::vmcast(&phv(arr));
  }

  static const ::hermes::vm::WeakRef<vm::HermesValue> &wrhv(
      const jsi::Pointer &pointer) {
    assert(
        dynamic_cast<const WeakRefPointerValue *>(getPointerValue(pointer)) &&
        "Pointer does not contain a WeakRefPointerValue");
    return static_cast<const WeakRefPointerValue *>(getPointerValue(pointer))
        ->wr;
  }

  // These helpers use public (mostly) interfaces on the runtime and
  // value types to convert between jsi and vm types.

  static vm::HermesValue hvFromValue(const jsi::Value &value) {
    if (value.isUndefined()) {
      return vm::HermesValue::encodeUndefinedValue();
    } else if (value.isNull()) {
      return vm::HermesValue::encodeNullValue();
    } else if (value.isBool()) {
      return vm::HermesValue::encodeBoolValue(value.getBool());
    } else if (value.isNumber()) {
      return vm::HermesValue::encodeNumberValue(value.getNumber());
    } else if (value.isString() || value.isObject()) {
      return phv(value);
    } else {
      llvm_unreachable("unknown value kind");
    }
  }

  vm::Handle<> vmHandleFromValue(const jsi::Value &value) {
    if (value.isUndefined()) {
      return runtime_.getUndefinedValue();
    } else if (value.isNull()) {
      return runtime_.getNullValue();
    } else if (value.isBool()) {
      return runtime_.getBoolValue(value.getBool());
    } else if (value.isNumber()) {
      return runtime_.makeHandle(
          vm::HermesValue::encodeNumberValue(value.getNumber()));
    } else if (value.isString() || value.isObject()) {
      return vm::Handle<vm::HermesValue>(&phv(value));
    } else {
      llvm_unreachable("unknown value kind");
    }
  }

  jsi::Value valueFromHermesValue(vm::HermesValue hv) {
    if (hv.isUndefined() || hv.isEmpty()) {
      return jsi::Value::undefined();
    } else if (hv.isNull()) {
      return nullptr;
    } else if (hv.isBool()) {
      return hv.getBool();
    } else if (hv.isDouble()) {
      return hv.getDouble();
    } else if (hv.isString()) {
      return add<jsi::String>(hv);
    } else if (hv.isObject()) {
      return add<jsi::Object>(hv);
    } else {
      llvm_unreachable("unknown HermesValue type");
    }
  }

  // Concrete declarations of jsi::Runtime pure virtual methods

  std::shared_ptr<const jsi::PreparedJavaScript> prepareJavaScript(
      const std::shared_ptr<const jsi::Buffer> &buffer,
      std::string sourceURL) override;
  void evaluatePreparedJavaScript(
      const std::shared_ptr<const jsi::PreparedJavaScript> &js) override;
  void evaluateJavaScript(
      const std::shared_ptr<const jsi::Buffer> &buffer,
      const std::string &sourceURL) override;
  jsi::Object global() override;

  std::string description() override;
  bool isInspectable() override;
  jsi::Instrumentation &instrumentation() override;

  PointerValue *cloneString(const Runtime::PointerValue *pv) override;
  PointerValue *cloneObject(const Runtime::PointerValue *pv) override;
  PointerValue *clonePropNameID(const Runtime::PointerValue *pv) override;

  jsi::PropNameID createPropNameIDFromAscii(const char *str, size_t length)
      override;
  jsi::PropNameID createPropNameIDFromUtf8(const uint8_t *utf8, size_t length)
      override;
  jsi::PropNameID createPropNameIDFromString(const jsi::String &str) override;
  std::string utf8(const jsi::PropNameID &) override;
  bool compare(const jsi::PropNameID &, const jsi::PropNameID &) override;

  jsi::String createStringFromAscii(const char *str, size_t length) override;
  jsi::String createStringFromUtf8(const uint8_t *utf8, size_t length) override;
  std::string utf8(const jsi::String &) override;

  jsi::Object createObject() override;
  jsi::Object createObject(std::shared_ptr<jsi::HostObject> ho) override;
  std::shared_ptr<jsi::HostObject> getHostObject(const jsi::Object &) override;
  jsi::HostFunctionType &getHostFunction(const jsi::Function &) override;
  jsi::Value getProperty(const jsi::Object &, const jsi::PropNameID &name)
      override;
  jsi::Value getProperty(const jsi::Object &, const jsi::String &name) override;
  bool hasProperty(const jsi::Object &, const jsi::PropNameID &name) override;
  bool hasProperty(const jsi::Object &, const jsi::String &name) override;
  void setPropertyValue(
      jsi::Object &,
      const jsi::PropNameID &name,
      const jsi::Value &value) override;
  void setPropertyValue(
      jsi::Object &,
      const jsi::String &name,
      const jsi::Value &value) override;
  bool isArray(const jsi::Object &) const override;
  bool isArrayBuffer(const jsi::Object &) const override;
  bool isFunction(const jsi::Object &) const override;
  bool isHostObject(const jsi::Object &) const override;
  bool isHostFunction(const jsi::Function &) const override;
  jsi::Array getPropertyNames(const jsi::Object &) override;

  jsi::WeakObject createWeakObject(const jsi::Object &) override;
  jsi::Value lockWeakObject(const jsi::WeakObject &) override;

  jsi::Array createArray(size_t length) override;
  size_t size(const jsi::Array &) override;
  size_t size(const jsi::ArrayBuffer &) override;
  uint8_t *data(const jsi::ArrayBuffer &) override;
  jsi::Value getValueAtIndex(const jsi::Array &, size_t i) override;
  void setValueAtIndexImpl(jsi::Array &, size_t i, const jsi::Value &value)
      override;

  jsi::Function createFunctionFromHostFunction(
      const jsi::PropNameID &name,
      unsigned int paramCount,
      jsi::HostFunctionType func) override;
  jsi::Value call(
      const jsi::Function &,
      const jsi::Value &jsThis,
      const jsi::Value *args,
      size_t count) override;
  jsi::Value callAsConstructor(
      const jsi::Function &,
      const jsi::Value *args,
      size_t count) override;

  bool strictEquals(const jsi::String &a, const jsi::String &b) const override;
  bool strictEquals(const jsi::Object &a, const jsi::Object &b) const override;

  bool instanceOf(const jsi::Object &o, const jsi::Function &ctor) override;

  ScopeState *pushScope() override;
  void popScope(ScopeState *prv) override;

  void checkStatus(vm::ExecutionStatus);
  vm::HermesValue stringHVFromUtf8(const uint8_t *utf8, size_t length);
  size_t getLength(vm::Handle<vm::ArrayImpl> arr);
  size_t getByteLength(vm::Handle<vm::JSArrayBuffer> arr);

  friend class jsi::detail::ThreadSafeRuntimeImpl<HermesRuntimeImpl, Lock>;

  struct JsiProxyBase : public vm::HostObjectProxy {
    JsiProxyBase(HermesRuntimeImpl &rt, std::shared_ptr<jsi::HostObject> ho)
        : rt_(rt), ho_(ho) {}

    HermesRuntimeImpl &rt_;
    std::shared_ptr<jsi::HostObject> ho_;
  };

  struct JsiProxy
#if !API_TRACE
      final
#endif
      : public JsiProxyBase {
    using JsiProxyBase::JsiProxyBase;
    vm::CallResult<vm::HermesValue> get(vm::SymbolID id) override {
      jsi::PropNameID sym =
          rt_.add<jsi::PropNameID>(vm::HermesValue::encodeSymbolValue(id));
      jsi::Value ret;
      try {
        ret = ho_->get(rt_, sym);
      } catch (const jsi::JSError &error) {
        return rt_.runtime_.setThrownValue(hvFromValue(error.value()));
      } catch (const std::exception &ex) {
        return rt_.runtime_.setThrownValue(
            hvFromValue(rt_.global()
                            .getPropertyAsFunction(rt_, "Error")
                            .call(
                                rt_,
                                std::string("Exception in HostObject::get: ") +
                                    ex.what())));
      } catch (...) {
        return rt_.runtime_.setThrownValue(hvFromValue(
            rt_.global()
                .getPropertyAsFunction(rt_, "Error")
                .call(rt_, "Exception in HostObject::get: <unknown>")));
      }

      return hvFromValue(ret);
    }

    vm::CallResult<bool> set(vm::SymbolID id, vm::HermesValue value) override {
      jsi::PropNameID sym =
          rt_.add<jsi::PropNameID>(vm::HermesValue::encodeSymbolValue(id));
      try {
        ho_->set(rt_, sym, rt_.valueFromHermesValue(value));
      } catch (const jsi::JSError &error) {
        return rt_.runtime_.setThrownValue(hvFromValue(error.value()));
      } catch (const std::exception &ex) {
        return rt_.runtime_.setThrownValue(
            hvFromValue(rt_.global()
                            .getPropertyAsFunction(rt_, "Error")
                            .call(
                                rt_,
                                std::string("Exception in HostObject::set: ") +
                                    ex.what())));
      } catch (...) {
        return rt_.runtime_.setThrownValue(hvFromValue(
            rt_.global()
                .getPropertyAsFunction(rt_, "Error")
                .call(rt_, "Exception in HostObject::set: <unknown>")));
      }
      return true;
    }

    vm::CallResult<vm::Handle<vm::JSArray>> getHostPropertyNames() override {
      try {
        auto names = ho_->getPropertyNames(rt_);

        auto arrayRes =
            vm::JSArray::create(&rt_.runtime_, names.size(), names.size());
        if (arrayRes == vm::ExecutionStatus::EXCEPTION) {
          return vm::ExecutionStatus::EXCEPTION;
        }
        vm::Handle<vm::JSArray> arrayHandle =
            vm::toHandle(&rt_.runtime_, std::move(*arrayRes));

        vm::GCScope gcScope{&rt_.runtime_};
        vm::MutableHandle<vm::SymbolID> tmpHandle{&rt_.runtime_};
        size_t i = 0;
        for (auto &name : names) {
          tmpHandle = phv(name).getSymbol();
          vm::JSArray::setElementAt(arrayHandle, &rt_.runtime_, i++, tmpHandle);
        }

        return arrayHandle;
      } catch (const jsi::JSError &error) {
        return rt_.runtime_.setThrownValue(hvFromValue(error.value()));
      } catch (const std::exception &ex) {
        return rt_.runtime_.setThrownValue(hvFromValue(
            rt_.global()
                .getPropertyAsFunction(rt_, "Error")
                .call(
                    rt_,
                    std::string("Exception in HostObject::getPropertyNames: ") +
                        ex.what())));
      } catch (...) {
        return rt_.runtime_.setThrownValue(hvFromValue(
            rt_.global()
                .getPropertyAsFunction(rt_, "Error")
                .call(
                    rt_,
                    "Exception in HostObject::getPropertyNames: <unknown>")));
      }
    };
  };

  struct HFContextBase {
    HFContextBase(jsi::HostFunctionType hf, HermesRuntimeImpl &hri)
        : hostFunction(std::move(hf)), hermesRuntimeImpl(hri) {}

    jsi::HostFunctionType hostFunction;
    HermesRuntimeImpl &hermesRuntimeImpl;
  };

  struct HFContext
#if !API_TRACE
      final
#endif
      : public HFContextBase {
    using HFContextBase::HFContextBase;

    static vm::CallResult<vm::HermesValue>
    func(void *context, vm::Runtime *runtime, vm::NativeArgs hvArgs) {
      HFContext *hfc = reinterpret_cast<HFContext *>(context);
      HermesRuntimeImpl &rt = hfc->hermesRuntimeImpl;
      assert(runtime == &rt.runtime_);
      auto &stats = rt.runtime_.getRuntimeStats();
      const vm::instrumentation::RAIITimer timer{
          "Host Function", stats, stats.hostFunction};

      llvm::SmallVector<jsi::Value, 8> apiArgs;
      for (vm::HermesValue hv : hvArgs) {
        apiArgs.push_back(rt.valueFromHermesValue(hv));
      }

      jsi::Value ret;
      const jsi::Value *args = apiArgs.empty() ? nullptr : &apiArgs.front();

      try {
        ret = (hfc->hostFunction)(
            rt,
            rt.valueFromHermesValue(hvArgs.getThisArg()),
            args,
            apiArgs.size());
      } catch (const jsi::JSError &error) {
        return runtime->setThrownValue(hvFromValue(error.value()));
      } catch (const std::exception &ex) {
        return rt.runtime_.setThrownValue(hvFromValue(
            rt.global()
                .getPropertyAsFunction(rt, "Error")
                .call(
                    rt,
                    std::string("Exception in HostFunction: ") + ex.what())));
      } catch (...) {
        return rt.runtime_.setThrownValue(
            hvFromValue(rt.global()
                            .getPropertyAsFunction(rt, "Error")
                            .call(rt, "Exception in HostFunction: <unknown>")));
      }

      return hvFromValue(ret);
    }

    static void finalize(void *context) {
      delete reinterpret_cast<HFContext *>(context);
    }
  };

  template <typename T>
  struct ManagedValues {
#ifndef NDEBUG
    ~ManagedValues() {
      for (const auto &s : values) {
        assert(
            s.get() == 0 &&
            "Runtime destroyed with outstanding API references");
      }
    }
#endif
    std::list<T> *operator->() {
      return &values;
    }

    const std::list<T> *operator->() const {
      return &values;
    }

    std::list<T> values;
  };

 protected:
  /// Helper function that is parameterized over the type of context being
  /// created.
  template <typename ContextType>
  jsi::Function createFunctionFromHostFunction(
      ContextType *context,
      const jsi::PropNameID &name,
      unsigned int paramCount);

 public:
  ManagedValues<HermesPointerValue> hermesValues_;
  ManagedValues<WeakRefPointerValue> weakHermesValues_;
#ifdef HERMESJSI_ON_STACK
  StackRuntime stackRuntime_;
#else
  std::shared_ptr<::hermes::vm::Runtime> rt_;
#endif
  ::hermes::vm::Runtime &runtime_;
#ifdef HERMES_ENABLE_DEBUGGER
  friend class debugger::Debugger;
  std::unique_ptr<debugger::Debugger> debugger_;
#endif
};

#if API_TRACE

SynthTrace::TraceValue toTraceValue(
    HermesRuntimeImpl &rt,
    SynthTrace &trace,
    const jsi::Value &value) {
  if (value.isUndefined()) {
    return SynthTrace::encodeUndefined();
  } else if (value.isNull()) {
    return SynthTrace::encodeNull();
  } else if (value.isBool()) {
    return SynthTrace::encodeBool(value.getBool());
  } else if (value.isNumber()) {
    return SynthTrace::encodeNumber(value.getNumber());
  } else if (value.isString()) {
    return trace.encodeString(rt.utf8(value.getString(rt)));
  } else if (value.isObject()) {
    // Get a unique identifier from the object, and use that instead. This is
    // so that object identity is tracked.
    return SynthTrace::encodeObject(rt.getUniqueID(value.getObject(rt)));
  } else {
    throw std::logic_error("Unsupported value reached");
  }
}

class TracingHermesRuntimeImpl : public HermesRuntimeImpl {
  std::vector<SynthTrace::TraceValue> argStringifyer(
      const jsi::Value *args,
      size_t count) {
    std::vector<SynthTrace::TraceValue> stringifiedArgs;
    stringifiedArgs.reserve(count);
    for (size_t i = 0; i < count; ++i) {
      stringifiedArgs.emplace_back(toTraceValue(*this, trace_, args[i]));
    }
    return stringifiedArgs;
  }

  struct TracingJsiProxy : public JsiProxy {
    TracingJsiProxy(
        TracingHermesRuntimeImpl &rt,
        std::shared_ptr<jsi::HostObject> ho)
        : JsiProxy(rt, ho), objID_(0) {}

    vm::CallResult<vm::HermesValue> get(vm::SymbolID id) override {
      auto &rt = static_cast<TracingHermesRuntimeImpl &>(rt_);
      // Convert the symbol to a serializable name.
      jsi::PropNameID sym =
          rt.add<jsi::PropNameID>(vm::HermesValue::encodeSymbolValue(id));
      rt.trace_.emplace_back<SynthTrace::GetPropertyNativeRecord>(
          rt.getTimeSinceStart(), objID_, rt.utf8(sym));
      auto jsiRes = JsiProxy::get(id);
      if (jsiRes == vm::ExecutionStatus::EXCEPTION) {
        // The trace currently has no way to model exceptions thrown from C++
        // code.
        ::hermes::hermes_fatal(
            "Exception happened in native code during trace");
      }
      rt.trace_.emplace_back<SynthTrace::GetPropertyNativeReturnRecord>(
          rt.getTimeSinceStart(),
          toTraceValue(
              rt, rt.trace_, rt.valueFromHermesValue(jsiRes.getValue())));
      return jsiRes;
    }
    vm::CallResult<bool> set(vm::SymbolID id, vm::HermesValue value) override {
      auto &rt = static_cast<TracingHermesRuntimeImpl &>(rt_);
      // Convert the symbol to a serializable name.
      jsi::PropNameID sym =
          rt.add<jsi::PropNameID>(vm::HermesValue::encodeSymbolValue(id));
      rt.trace_.emplace_back<SynthTrace::SetPropertyNativeRecord>(
          rt.getTimeSinceStart(),
          objID_,
          rt.utf8(sym),
          toTraceValue(rt, rt.trace_, rt.valueFromHermesValue(value)));
      auto result = JsiProxy::set(id, value);
      if (result == vm::ExecutionStatus::EXCEPTION) {
        // The trace currently has no way to model exceptions thrown from C++
        // code.
        ::hermes::hermes_fatal(
            "Exception happened in native code during trace");
      }
      rt.trace_.emplace_back<SynthTrace::SetPropertyNativeReturnRecord>(
          rt.getTimeSinceStart());
      return result;
    }
    vm::CallResult<vm::Handle<vm::JSArray>> getHostPropertyNames() override {
      // It is not important to track this event, since it cannot modify the
      // heap.
      auto result = JsiProxy::getHostPropertyNames();
      if (result == vm::ExecutionStatus::EXCEPTION) {
        // The trace currently has no way to model exceptions thrown from C++
        // code.
        ::hermes::hermes_fatal(
            "Exception happened in native code during trace");
      }
      return result;
    }

    void setObjectID(SynthTrace::ObjectID id) {
      objID_ = id;
    }

    /// The object id of the host object that this is attached to.
    SynthTrace::ObjectID objID_;
  };

  struct TracingHFContext final : public HFContext {
    TracingHFContext(jsi::HostFunctionType hf, TracingHermesRuntimeImpl &hri)
        : HFContext(hf, hri), functionID_(0) {}

    static vm::CallResult<vm::HermesValue>
    func(void *context, vm::Runtime *runtime, vm::NativeArgs hvArgs) {
      auto *hfc = reinterpret_cast<TracingHFContext *>(context);
      auto &rt =
          static_cast<TracingHermesRuntimeImpl &>(hfc->hermesRuntimeImpl);
      llvm::SmallVector<jsi::Value, 8> apiArgs;
      for (vm::HermesValue hv : hvArgs) {
        apiArgs.push_back(rt.valueFromHermesValue(hv));
      }
      rt.trace_.emplace_back<SynthTrace::CallToNativeRecord>(
          rt.getTimeSinceStart(),
          hfc->functionID_,
          // A host function does not have a this.
          SynthTrace::encodeUndefined(),
          rt.argStringifyer(
              apiArgs.empty() ? nullptr : &apiArgs.front(), apiArgs.size()));
      auto value = HFContext::func(context, runtime, hvArgs);
      if (value == vm::ExecutionStatus::EXCEPTION) {
        // The trace currently has no way to model exceptions thrown from C++
        // code.
        ::hermes::hermes_fatal(
            "Exception happened in native code during trace");
      }
      rt.trace_.emplace_back<SynthTrace::ReturnFromNativeRecord>(
          rt.getTimeSinceStart(),
          toTraceValue(
              rt, rt.trace_, rt.valueFromHermesValue(value.getValue())));
      return value;
    }

    static void finalize(void *context) {
      delete reinterpret_cast<TracingHFContext *>(context);
    }

    void setFunctionID(SynthTrace::ObjectID functionID) {
      functionID_ = functionID;
    }

    /// The object id of the function that this is attached to.
    SynthTrace::ObjectID functionID_;
  };

 public:
  TracingHermesRuntimeImpl(const vm::RuntimeConfig &runtimeConfig)
      : HermesRuntimeImpl(runtimeConfig),
        trace_(getUniqueID(global())),
        conf_(runtimeConfig) {}

  /// @name jsi::Runtime methods.
  /// @{

  void evaluateJavaScript(
      const std::shared_ptr<const jsi::Buffer> &buffer,
      const std::string &sourceURL) override {
    if (isHermesBytecode(buffer->data(), buffer->size())) {
      trace_.setSourceHash(hbc::BCProviderFromBuffer::getSourceHashFromBytecode(
          llvm::makeArrayRef(buffer->data(), buffer->size())));
    }
    trace_.emplace_back<SynthTrace::BeginExecJSRecord>(getTimeSinceStart());
    HermesRuntimeImpl::evaluateJavaScript(buffer, sourceURL);
    trace_.emplace_back<SynthTrace::EndExecJSRecord>(getTimeSinceStart());
  }

  jsi::Object createObject() override {
    auto obj = HermesRuntimeImpl::createObject();
    trace_.emplace_back<SynthTrace::CreateObjectRecord>(
        getTimeSinceStart(), getUniqueID(obj));
    return obj;
  }

  jsi::Object createObject(std::shared_ptr<jsi::HostObject> ho) override {
    vm::GCScope gcScope(&runtime_);
    auto proxy = std::make_shared<TracingJsiProxy>(*this, ho);
    auto objRes = vm::HostObject::createWithoutPrototype(&runtime_, proxy);
    checkStatus(objRes.getStatus());
    auto obj = add<jsi::Object>(*objRes);
    // Track the unique id of the HostObject in the proxy so that the proxy
    // also has a unique identifier.
    proxy->setObjectID(getUniqueID(obj));
    trace_.emplace_back<SynthTrace::CreateHostObjectRecord>(
        getTimeSinceStart(), getUniqueID(obj));
    return obj;
  }

  jsi::Value getProperty(const jsi::Object &obj, const jsi::String &name)
      override {
    auto value = HermesRuntimeImpl::getProperty(obj, name);
    trace_.emplace_back<SynthTrace::GetPropertyRecord>(
        getTimeSinceStart(),
        getUniqueID(obj),
        utf8(name),
        toTraceValue(*this, trace_, value));
    return value;
  }

  jsi::Value getProperty(const jsi::Object &obj, const jsi::PropNameID &name)
      override {
    auto value = HermesRuntimeImpl::getProperty(obj, name);
    trace_.emplace_back<SynthTrace::GetPropertyRecord>(
        getTimeSinceStart(),
        getUniqueID(obj),
        utf8(name),
        toTraceValue(*this, trace_, value));
    return value;
  }

  bool hasProperty(const jsi::Object &obj, const jsi::String &name) override {
    trace_.emplace_back<SynthTrace::HasPropertyRecord>(
        getTimeSinceStart(), getUniqueID(obj), utf8(name));
    return HermesRuntimeImpl::hasProperty(obj, name);
  }

  bool hasProperty(const jsi::Object &obj, const jsi::PropNameID &name)
      override {
    trace_.emplace_back<SynthTrace::HasPropertyRecord>(
        getTimeSinceStart(), getUniqueID(obj), utf8(name));
    return HermesRuntimeImpl::hasProperty(obj, name);
  }

  void setPropertyValue(
      jsi::Object &obj,
      const jsi::String &name,
      const jsi::Value &value) override {
    trace_.emplace_back<SynthTrace::SetPropertyRecord>(
        getTimeSinceStart(),
        getUniqueID(obj),
        utf8(name),
        toTraceValue(*this, trace_, value));
    HermesRuntimeImpl::setPropertyValue(obj, name, value);
  }

  void setPropertyValue(
      jsi::Object &obj,
      const jsi::PropNameID &name,
      const jsi::Value &value) override {
    trace_.emplace_back<SynthTrace::SetPropertyRecord>(
        getTimeSinceStart(),
        getUniqueID(obj),
        utf8(name),
        toTraceValue(*this, trace_, value));
    HermesRuntimeImpl::setPropertyValue(obj, name, value);
  }

  jsi::WeakObject createWeakObject(const jsi::Object &o) override {
    auto wo = HermesRuntimeImpl::createWeakObject(o);
    // TODO mhorowitz: add synthtrace support for WeakObject
    return wo;
  }

  jsi::Value lockWeakObject(const jsi::WeakObject &wo) override {
    auto val = HermesRuntimeImpl::lockWeakObject(wo);
    // TODO mhorowitz: add synthtrace support for WeakObject
    return val;
  }

  jsi::Array createArray(size_t length) override {
    auto arr = HermesRuntimeImpl::createArray(length);
    trace_.emplace_back<SynthTrace::CreateArrayRecord>(
        getTimeSinceStart(), getUniqueID(arr), length);
    return arr;
  }

  size_t size(const jsi::Array &arr) override {
    // Array size inquiries read from the length property, which is
    // non-configurable and thus cannot have side effects.
    return HermesRuntimeImpl::size(arr);
  }

  size_t size(const jsi::ArrayBuffer &buf) override {
    // ArrayBuffer size inquiries read from the byteLength property, which is
    // non-configurable and thus cannot have side effects.
    return HermesRuntimeImpl::size(buf);
  }

  uint8_t *data(const jsi::ArrayBuffer &buf) override {
    throw std::logic_error(
        "Cannot write raw bytes into an ArrayBuffer in trace mode");
  }

  jsi::Value getValueAtIndex(const jsi::Array &arr, size_t i) override {
    auto value = HermesRuntimeImpl::getValueAtIndex(arr, i);
    trace_.emplace_back<SynthTrace::ArrayReadRecord>(
        getTimeSinceStart(),
        getUniqueID(arr),
        i,
        toTraceValue(*this, trace_, value));
    return value;
  }

  void setValueAtIndexImpl(jsi::Array &arr, size_t i, const jsi::Value &value)
      override {
    trace_.emplace_back<SynthTrace::ArrayWriteRecord>(
        getTimeSinceStart(),
        getUniqueID(arr),
        i,
        toTraceValue(*this, trace_, value));
    return HermesRuntimeImpl::setValueAtIndexImpl(arr, i, value);
  }

  jsi::Function createFunctionFromHostFunction(
      const jsi::PropNameID &name,
      unsigned int paramCount,
      jsi::HostFunctionType func) override {
    auto context =
        ::hermes::make_unique<TracingHFContext>(std::move(func), *this);
    auto hostfunc = HermesRuntimeImpl::createFunctionFromHostFunction(
        context.get(), name, paramCount);
    context->setFunctionID(getUniqueID(hostfunc));
    context.release();
    trace_.emplace_back<SynthTrace::CreateHostFunctionRecord>(
        getTimeSinceStart(), getUniqueID(hostfunc));
    return hostfunc;
  }

  jsi::Value call(
      const jsi::Function &func,
      const jsi::Value &jsThis,
      const jsi::Value *args,
      size_t count) override {
    trace_.emplace_back<SynthTrace::CallFromNativeRecord>(
        getTimeSinceStart(),
        getUniqueID(func),
        toTraceValue(*this, trace_, jsThis),
        argStringifyer(args, count));
    auto retval = HermesRuntimeImpl::call(func, jsThis, args, count);
    trace_.emplace_back<SynthTrace::ReturnToNativeRecord>(
        getTimeSinceStart(), toTraceValue(*this, trace_, retval));
    return retval;
  }

  jsi::Value callAsConstructor(
      const jsi::Function &func,
      const jsi::Value *args,
      size_t count) override {
    trace_.emplace_back<SynthTrace::ConstructFromNativeRecord>(
        getTimeSinceStart(),
        getUniqueID(func),
        // A construct call always has an undefined this.
        // The ReturnToNativeRecord will contain the object that was either
        // created by the new keyword, or the objec that's returned from the
        // function.
        SynthTrace::encodeUndefined(),
        argStringifyer(args, count));
    auto retval = HermesRuntimeImpl::callAsConstructor(func, args, count);
    trace_.emplace_back<SynthTrace::ReturnToNativeRecord>(
        getTimeSinceStart(), toTraceValue(*this, trace_, retval));
    return retval;
  }

  /// @}

  /// @name jsi::Instrumentation methods
  /// @{

  void writeBridgeTrafficTraceToFile(
      const std::string &fileName) const override {
    std::error_code ec;
    llvm::raw_fd_ostream fs{fileName.c_str(), ec, llvm::sys::fs::F_Text};
    if (ec) {
      throw std::system_error(ec);
    }
    HermesRuntime::writeTrace(fs);
  }

  /// @}

  void addTTIMarker() {
    trace_.emplace_back<SynthTrace::MarkerRecord>(getTimeSinceStart(), "tti");
  }

 private:
  SynthTrace::TimeSinceStart getTimeSinceStart() const {
    return std::chrono::duration_cast<SynthTrace::TimeSinceStart>(
        std::chrono::steady_clock::now() - startTime_);
  }

 public:
  SynthTrace trace_;
  const vm::RuntimeConfig conf_;
  const SynthTrace::TimePoint startTime_{std::chrono::steady_clock::now()};
};
#endif

#ifdef HERMES_ENABLE_DEBUGGER

inline HermesRuntimeImpl *impl(HermesRuntime *rt) {
  // This is guaranteed safe because HermesRuntime is abstract so
  // cannot be constructed, and the only instances created are
  // HermesRuntimeImpl's created by the factory function.  It's kind
  // of like pimpl, but different.

  return static_cast<HermesRuntimeImpl *>(rt);
}

#endif

inline const HermesRuntimeImpl *impl(const HermesRuntime *rt) {
  // See above comment

  return static_cast<const HermesRuntimeImpl *>(rt);
}

} // namespace

bool HermesRuntime::isHermesBytecode(const uint8_t *data, size_t len) {
  return hbc::BCProviderFromBuffer::isBytecodeStream(
      llvm::ArrayRef<uint8_t>(data, len));
}

void HermesRuntime::prefetchHermesBytecode(const uint8_t *data, size_t len) {
  hbc::BCProviderFromBuffer::prefetch(llvm::ArrayRef<uint8_t>(data, len));
}

bool HermesRuntime::hermesBytecodeSanityCheck(
    const uint8_t *data,
    size_t len,
    std::string *errorMessage) {
  return hbc::BCProviderFromBuffer::bytecodeStreamSanityCheck(
      llvm::ArrayRef<uint8_t>(data, len), errorMessage);
}

std::pair<const uint8_t *, size_t> HermesRuntime::getBytecodeEpilogue(
    const uint8_t *data,
    size_t len) {
  auto epi = hbc::BCProviderFromBuffer::getEpilogueFromBytecode(
      llvm::ArrayRef<uint8_t>(data, len));
  return std::make_pair(epi.data(), epi.size());
}

void HermesRuntime::enableSamplingProfiler() {
  ::hermes::vm::SamplingProfiler::getInstance()->enable();
}

void HermesRuntime::dumpSampledTraceToFile(const std::string &fileName) {
  std::error_code ec;
  llvm::raw_fd_ostream os(fileName.c_str(), ec, llvm::sys::fs::F_Text);
  if (ec) {
    throw std::system_error(ec);
  }
  ::hermes::vm::SamplingProfiler::getInstance()->dumpChromeTrace(os);
}

void HermesRuntime::setFatalHandler(void (*handler)(const std::string &)) {
  detail::sApiFatalHandler = handler;
}

#if API_TRACE
SynthTrace &HermesRuntime::trace() {
  return static_cast<TracingHermesRuntimeImpl *>(this)->trace_;
}

void HermesRuntime::writeTrace(llvm::raw_ostream &os) const {
  const auto *self = static_cast<const TracingHermesRuntimeImpl *>(this);
  os << SynthTrace::Printable(
      self->trace_, self->runtime_.getCommonStorage()->tracedEnv, self->conf_);
}

SynthTrace::ObjectID HermesRuntime::getUniqueID(const jsi::Object &o) const {
  return static_cast<vm::GCCell *>(impl(this)->phv(o).getObject())
      ->getDebugAllocationId();
}

#endif

#ifdef HERMESVM_SYNTH_REPLAY
void HermesRuntime::setMockedEnvironment(
    const ::hermes::vm::MockedEnvironment &env) {
  static_cast<HermesRuntimeImpl *>(this)->runtime_.setMockedEnvironment(env);
}
#endif

#ifdef HERMESVM_PROFILER_BB
void HermesRuntime::dumpBasicBlockProfileTrace(llvm::raw_ostream &os) const {
  static_cast<const HermesRuntimeImpl *>(this)
      ->runtime_.dumpBasicBlockProfileTrace(os);
}
#endif

#ifdef HERMESVM_PROFILER_OPCODE
void HermesRuntime::dumpOpcodeStats(llvm::raw_ostream &os) const {
  static_cast<const HermesRuntimeImpl *>(this)->runtime_.dumpOpcodeStats(os);
}
#endif

#ifdef HERMES_ENABLE_DEBUGGER

debugger::Debugger &HermesRuntime::getDebugger() {
  return *(impl(this)->debugger_);
}

void HermesRuntime::debugJavaScript(
    const std::string &src,
    const std::string &sourceURL,
    const DebugFlags &debugFlags) {
  vm::Runtime &runtime = impl(this)->runtime_;
  vm::GCScope gcScope(&runtime);
  hbc::CompileFlags flags{};
  flags.debug = true;
  flags.lazy = debugFlags.lazy;
  vm::ExecutionStatus res = runtime.run(src, sourceURL, flags);
  impl(this)->checkStatus(res);
}
#endif

#ifdef HERMESVM_PLATFORM_LOGGING
namespace {

template <typename Runtime>
void logGCStats(Runtime &rt, const char *msg) {
  // The GC stats can exceed the android logcat length limit, of
  // 1024 bytes.  Break it up.
  std::string stats = rt.instrumentation().getRecordedGCStats();
  auto copyRegionFrom = [&stats](size_t from) -> size_t {
    size_t rBrace = stats.find("},", from);
    if (rBrace == std::string::npos) {
      std::string portion = stats.substr(from);
      hermesLog("HermesVM", "%s", portion.c_str());
      return stats.size();
    }

    // Add 2 for the length of the search string, to get to the end.
    const size_t to = rBrace + 2;
    std::string portion = stats.substr(from, to - from);
    hermesLog("HermesVM", "%s", portion.c_str());
    return to;
  };

  hermesLog("HermesVM", "%s:", msg);
  for (size_t ind = 0; ind < stats.size(); ind = copyRegionFrom(ind))
    ;
}

} // namespace
#endif

size_t HermesRuntime::rootsListLength() const {
  return impl(this)->hermesValues_->size();
}

namespace {

/// An implementation of PreparedJavaScript that wraps a BytecodeProvider.
class HermesPreparedJavaScript final : public jsi::PreparedJavaScript {
  std::shared_ptr<hbc::BCProvider> bcProvider_;
  vm::RuntimeModuleFlags runtimeFlags_;
  std::string sourceURL_;

 public:
  explicit HermesPreparedJavaScript(
      std::unique_ptr<hbc::BCProvider> bcProvider,
      vm::RuntimeModuleFlags runtimeFlags,
      std::string sourceURL)
      : bcProvider_(std::move(bcProvider)),
        runtimeFlags_(runtimeFlags),
        sourceURL_(std::move(sourceURL)) {}

  std::shared_ptr<hbc::BCProvider> bytecodeProvider() const {
    return bcProvider_;
  }

  vm::RuntimeModuleFlags runtimeFlags() const {
    return runtimeFlags_;
  }

  const std::string &sourceURL() const {
    return sourceURL_;
  }
};

// A class which adapts a jsi buffer to a Hermes buffer.
class BufferAdapter final : public ::hermes::Buffer {
 public:
  BufferAdapter(std::shared_ptr<const jsi::Buffer> buf) : buf_(std::move(buf)) {
    data_ = buf_->data();
    size_ = buf_->size();
  }

 private:
  std::shared_ptr<const jsi::Buffer> buf_;
};

} // namespace

std::shared_ptr<const jsi::PreparedJavaScript>
HermesRuntimeImpl::prepareJavaScript(
    const std::shared_ptr<const jsi::Buffer> &jsiBuffer,
    std::string sourceURL) {
  std::pair<std::unique_ptr<hbc::BCProvider>, std::string> bcErr{};
  auto buffer = std::make_unique<BufferAdapter>(std::move(jsiBuffer));
  vm::RuntimeModuleFlags runtimeFlags{};
  runtimeFlags.persistent = true;

  bool isBytecode = isHermesBytecode(buffer->data(), buffer->size());
#ifdef HERMESVM_PLATFORM_LOGGING
  hermesLog(
      "HermesVM", "Prepare JS on %s.", isBytecode ? "bytecode" : "source");
#endif

  // Construct the BC provider either from buffer or source.
  if (isBytecode) {
    bcErr = hbc::BCProviderFromBuffer::createBCProviderFromBuffer(
        std::move(buffer));
  } else {
    hbc::CompileFlags compileFlags{};
    compileFlags.optimize = false;
    compileFlags.lazy = (buffer->size() >= kMinimumLazySize);
#ifdef HERMES_ENABLE_DEBUGGER
    compileFlags.debug = true;
#endif
#if defined(HERMESVM_LEAN)
    bcErr.second = "prepareJavaScript source compilation not supported";
#else
    bcErr = hbc::BCProviderFromSrc::createBCProviderFromSrc(
        std::move(buffer), sourceURL, compileFlags);
#endif
  }
  if (!bcErr.first) {
    throw jsi::JSINativeException(std::move(bcErr.second));
  }
  return std::make_shared<const HermesPreparedJavaScript>(
      std::move(bcErr.first), runtimeFlags, std::move(sourceURL));
}

void HermesRuntimeImpl::evaluatePreparedJavaScript(
    const std::shared_ptr<const jsi::PreparedJavaScript> &js) {
  assert(
      dynamic_cast<const HermesPreparedJavaScript *>(js.get()) &&
      "js must be an instance of HermesPreparedJavaScript");
  ::hermes::instrumentation::HighFreqPerfMarker m("jsi-hermes-evaluate");
  auto &stats = runtime_.getRuntimeStats();
  const vm::instrumentation::RAIITimer timer{
      "Evaluate JS", stats, stats.evaluateJS};
  const auto *hermesPrep =
      static_cast<const HermesPreparedJavaScript *>(js.get());
  vm::GCScope gcScope(&runtime_);
  checkStatus(runtime_.runBytecode(
      hermesPrep->bytecodeProvider(),
      hermesPrep->runtimeFlags(),
      hermesPrep->sourceURL(),
      runtime_.makeNullHandle<vm::Environment>()));
}

void HermesRuntimeImpl::evaluateJavaScript(
    const std::shared_ptr<const jsi::Buffer> &buffer,
    const std::string &sourceURL) {
  evaluatePreparedJavaScript(prepareJavaScript(buffer, sourceURL));
}

jsi::Object HermesRuntimeImpl::global() {
  return add<jsi::Object>(runtime_.getGlobal().getHermesValue());
}

std::string HermesRuntimeImpl::description() {
  return runtime_.getHeap().getName();
}

bool HermesRuntimeImpl::isInspectable() {
#ifdef HERMES_ENABLE_DEBUGGER
  return true;
#else
  return false;
#endif
}

jsi::Instrumentation &HermesRuntimeImpl::instrumentation() {
  return *this;
}

jsi::Runtime::PointerValue *HermesRuntimeImpl::cloneString(
    const Runtime::PointerValue *pv) {
  return clone(pv);
}

jsi::Runtime::PointerValue *HermesRuntimeImpl::cloneObject(
    const Runtime::PointerValue *pv) {
  return clone(pv);
}

jsi::Runtime::PointerValue *HermesRuntimeImpl::clonePropNameID(
    const Runtime::PointerValue *pv) {
  return clone(pv);
}

jsi::PropNameID HermesRuntimeImpl::createPropNameIDFromAscii(
    const char *str,
    size_t length) {
#ifndef NDEBUG
  for (size_t i = 0; i < length; ++i) {
    assert(
        static_cast<unsigned char>(str[i]) < 128 &&
        "non-ASCII character in property name");
  }
#endif

  vm::GCScope gcScope(&runtime_);
  auto cr = vm::stringToSymbolID(
      &runtime_,
      vm::StringPrimitive::createNoThrow(
          &runtime_, llvm::StringRef(str, length)));
  checkStatus(cr.getStatus());
  return add<jsi::PropNameID>(cr->getHermesValue());
}

jsi::PropNameID HermesRuntimeImpl::createPropNameIDFromUtf8(
    const uint8_t *utf8,
    size_t length) {
  vm::GCScope gcScope(&runtime_);
  auto cr = vm::stringToSymbolID(
      &runtime_,
      vm::createPseudoHandle(stringHVFromUtf8(utf8, length).getString()));
  checkStatus(cr.getStatus());
  return add<jsi::PropNameID>(cr->getHermesValue());
}

jsi::PropNameID HermesRuntimeImpl::createPropNameIDFromString(
    const jsi::String &str) {
  vm::GCScope gcScope(&runtime_);
  auto cr = vm::stringToSymbolID(
      &runtime_, vm::createPseudoHandle(phv(str).getString()));
  checkStatus(cr.getStatus());
  return add<jsi::PropNameID>(cr->getHermesValue());
}

std::string HermesRuntimeImpl::utf8(const jsi::PropNameID &sym) {
  vm::GCScope gcScope(&runtime_);
  vm::SymbolID id = phv(sym).getSymbol();
  auto view = runtime_.getIdentifierTable().getStringView(&runtime_, id);
  vm::SmallU16String<32> allocator;
  std::string ret;
  ::hermes::convertUTF16ToUTF8WithReplacements(
      ret, view.getUTF16Ref(allocator));
  return ret;
}

bool HermesRuntimeImpl::compare(
    const jsi::PropNameID &a,
    const jsi::PropNameID &b) {
  return phv(a).getSymbol() == phv(b).getSymbol();
}

jsi::String HermesRuntimeImpl::createStringFromAscii(
    const char *str,
    size_t length) {
#ifndef NDEBUG
  for (size_t i = 0; i < length; ++i) {
    assert(
        static_cast<unsigned char>(str[i]) < 128 &&
        "non-ASCII character in string");
  }
#endif

  vm::GCScope gcScope(&runtime_);
  return add<jsi::String>(vm::StringPrimitive::createNoThrow(
                              &runtime_, llvm::StringRef(str, length))
                              .getHermesValue());
}

jsi::String HermesRuntimeImpl::createStringFromUtf8(
    const uint8_t *utf8,
    size_t length) {
  vm::GCScope gcScope(&runtime_);
  return add<jsi::String>(stringHVFromUtf8(utf8, length));
}

std::string HermesRuntimeImpl::utf8(const jsi::String &str) {
  vm::GCScope gcScope(&runtime_);
  vm::Handle<vm::StringPrimitive> handle(
      &runtime_, stringHandle(str)->getString());
  auto view = vm::StringPrimitive::createStringView(&runtime_, handle);
  vm::SmallU16String<32> allocator;
  std::string ret;
  ::hermes::convertUTF16ToUTF8WithReplacements(
      ret, view.getUTF16Ref(allocator));
  return ret;
}

jsi::Object HermesRuntimeImpl::createObject() {
  vm::GCScope gcScope(&runtime_);
  return add<jsi::Object>(vm::JSObject::create(&runtime_).getHermesValue());
}

jsi::Object HermesRuntimeImpl::createObject(
    std::shared_ptr<jsi::HostObject> ho) {
  vm::GCScope gcScope(&runtime_);

  auto objRes = vm::HostObject::createWithoutPrototype(
      &runtime_, std::make_shared<JsiProxy>(*this, ho));
  checkStatus(objRes.getStatus());
  return add<jsi::Object>(*objRes);
}

std::shared_ptr<jsi::HostObject> HermesRuntimeImpl::getHostObject(
    const jsi::Object &obj) {
  return std::static_pointer_cast<JsiProxyBase>(
             vm::vmcast<vm::HostObject>(phv(obj))->getProxy())
      ->ho_;
}

jsi::Value HermesRuntimeImpl::getProperty(
    const jsi::Object &obj,
    const jsi::String &name) {
  ::hermes::instrumentation::PerfMarker m("jsi-hermes-getProperty-string");
  vm::GCScope gcScope(&runtime_);
  auto h = handle(obj);
  auto res = h->getComputed(h, &runtime_, stringHandle(name));
  checkStatus(res.getStatus());
  return valueFromHermesValue(*res);
}

jsi::Value HermesRuntimeImpl::getProperty(
    const jsi::Object &obj,
    const jsi::PropNameID &name) {
  ::hermes::instrumentation::LowFreqPerfMarker m(
      "jsi-hermes-getProperty-nameid");
  vm::GCScope gcScope(&runtime_);
  auto h = handle(obj);
  vm::SymbolID nameID = phv(name).getSymbol();
  auto res = h->getNamedOrIndexed(h, &runtime_, nameID);
  checkStatus(res.getStatus());
  return valueFromHermesValue(*res);
}

bool HermesRuntimeImpl::hasProperty(
    const jsi::Object &obj,
    const jsi::String &name) {
  vm::GCScope gcScope(&runtime_);
  auto h = handle(obj);
  auto result = h->hasComputed(h, &runtime_, stringHandle(name));
  checkStatus(result.getStatus());
  return result.getValue();
}

bool HermesRuntimeImpl::hasProperty(
    const jsi::Object &obj,
    const jsi::PropNameID &name) {
  vm::GCScope gcScope(&runtime_);
  auto h = handle(obj);
  vm::SymbolID nameID = phv(name).getSymbol();
  return h->hasNamedOrIndexed(h, &runtime_, nameID);
}

void HermesRuntimeImpl::setPropertyValue(
    jsi::Object &obj,
    const jsi::String &name,
    const jsi::Value &value) {
  vm::GCScope gcScope(&runtime_);
  auto h = handle(obj);
  checkStatus(h->putComputed(
                   h,
                   &runtime_,
                   stringHandle(name),
                   vmHandleFromValue(value),
                   vm::PropOpFlags().plusThrowOnError())
                  .getStatus());
}

void HermesRuntimeImpl::setPropertyValue(
    jsi::Object &obj,
    const jsi::PropNameID &name,
    const jsi::Value &value) {
  vm::GCScope gcScope(&runtime_);
  auto h = handle(obj);
  vm::SymbolID nameID = phv(name).getSymbol();
  checkStatus(h->putNamedOrIndexed(
                   h,
                   &runtime_,
                   nameID,
                   vmHandleFromValue(value),
                   vm::PropOpFlags().plusThrowOnError())
                  .getStatus());
}

bool HermesRuntimeImpl::isArray(const jsi::Object &obj) const {
  return vm::vmisa<vm::JSArray>(phv(obj));
}

bool HermesRuntimeImpl::isArrayBuffer(const jsi::Object &obj) const {
  return vm::vmisa<vm::JSArrayBuffer>(phv(obj));
}

bool HermesRuntimeImpl::isFunction(const jsi::Object &obj) const {
  return vm::vmisa<vm::Callable>(phv(obj));
}

bool HermesRuntimeImpl::isHostObject(const jsi::Object &obj) const {
  return vm::vmisa<vm::HostObject>(phv(obj));
}

bool HermesRuntimeImpl::isHostFunction(const jsi::Function &func) const {
  return vm::vmisa<vm::FinalizableNativeFunction>(phv(func));
}

jsi::Array HermesRuntimeImpl::getPropertyNames(const jsi::Object &obj) {
  vm::GCScope gcScope(&runtime_);
  uint32_t beginIndex;
  uint32_t endIndex;
  vm::CallResult<vm::Handle<vm::SegmentedArray>> cr =
      vm::getForInPropertyNames(&runtime_, handle(obj), beginIndex, endIndex);
  checkStatus(cr.getStatus());
  vm::Handle<vm::SegmentedArray> arr = *cr;
  size_t length = endIndex - beginIndex;

  auto ret = createArray(length);
  for (size_t i = 0; i < length; ++i) {
    vm::HermesValue name = arr->at(beginIndex + i);
    if (name.isString()) {
      ret.setValueAtIndex(*this, i, valueFromHermesValue(name));
    } else if (name.isNumber()) {
      std::string s;
      llvm::raw_string_ostream os(s);
      os << static_cast<size_t>(name.getNumber());
      ret.setValueAtIndex(
          *this, i, jsi::String::createFromAscii(*this, os.str()));
    } else {
      llvm_unreachable("property name is not String or Number");
    }
  }

  return ret;
}

jsi::WeakObject HermesRuntimeImpl::createWeakObject(const jsi::Object &obj) {
  return addWeak(vm::WeakRef<vm::HermesValue>(&(runtime_.getHeap()), phv(obj)));
}

jsi::Value HermesRuntimeImpl::lockWeakObject(const jsi::WeakObject &wo) {
  const vm::WeakRef<vm::HermesValue> &wr = wrhv(wo);
  if (!wr.isValid()) {
    return jsi::Value();
  }

  vm::HermesValue hv = wr.unsafeGetHermesValue();
  assert(hv.isObject() && "jsi::WeakObject referent is not an Object");
  return add<jsi::Object>(hv);
}

jsi::Array HermesRuntimeImpl::createArray(size_t length) {
  vm::GCScope gcScope(&runtime_);
  auto result = vm::JSArray::create(&runtime_, length, length);
  checkStatus(result.getStatus());
  return add<jsi::Object>(result->getHermesValue()).getArray(*this);
}

size_t HermesRuntimeImpl::size(const jsi::Array &arr) {
  vm::GCScope gcScope(&runtime_);
  return getLength(arrayHandle(arr));
}

size_t HermesRuntimeImpl::size(const jsi::ArrayBuffer &arr) {
  vm::GCScope gcScope(&runtime_);
  return getByteLength(arrayBufferHandle(arr));
}

uint8_t *HermesRuntimeImpl::data(const jsi::ArrayBuffer &arr) {
  return reinterpret_cast<uint8_t *>(
      vm::vmcast<vm::JSArrayBuffer>(phv(arr))->getDataBlock());
}

jsi::Value HermesRuntimeImpl::getValueAtIndex(const jsi::Array &arr, size_t i) {
  vm::GCScope gcScope(&runtime_);
  if (LLVM_UNLIKELY(i >= size(arr))) {
    throw makeJSError(
        *this,
        "getValueAtIndex: index ",
        i,
        " is out of bounds [0, ",
        size(arr),
        ")");
  }

  auto res = vm::JSObject::getComputed(
      arrayHandle(arr),
      &runtime_,
      runtime_.makeHandle(vm::HermesValue::encodeNumberValue(i)));
  checkStatus(res.getStatus());

  return valueFromHermesValue(*res);
}

void HermesRuntimeImpl::setValueAtIndexImpl(
    jsi::Array &arr,
    size_t i,
    const jsi::Value &value) {
  vm::GCScope gcScope(&runtime_);
  if (LLVM_UNLIKELY(i >= size(arr))) {
    throw makeJSError(
        *this,
        "setValueAtIndex: index ",
        i,
        " is out of bounds [0, ",
        size(arr),
        ")");
  }

  auto h = arrayHandle(arr);
  h->setElementAt(h, &runtime_, i, vmHandleFromValue(value));
}

jsi::Function HermesRuntimeImpl::createFunctionFromHostFunction(
    const jsi::PropNameID &name,
    unsigned int paramCount,
    jsi::HostFunctionType func) {
  auto context = ::hermes::make_unique<HFContext>(std::move(func), *this);
  auto hostfunc =
      createFunctionFromHostFunction(context.get(), name, paramCount);
  context.release();
  return hostfunc;
}

template <typename ContextType>
jsi::Function HermesRuntimeImpl::createFunctionFromHostFunction(
    ContextType *context,
    const jsi::PropNameID &name,
    unsigned int paramCount) {
  vm::GCScope gcScope(&runtime_);
  vm::SymbolID nameID = phv(name).getSymbol();
  auto funcRes = vm::FinalizableNativeFunction::createWithoutPrototype(
      &runtime_,
      context,
      &ContextType::func,
      &ContextType::finalize,
      nameID,
      paramCount);
  checkStatus(funcRes.getStatus());
  jsi::Function ret = add<jsi::Object>(*funcRes).getFunction(*this);
  return ret;
}

jsi::HostFunctionType &HermesRuntimeImpl::getHostFunction(
    const jsi::Function &func) {
  return static_cast<HFContextBase *>(
             vm::vmcast<vm::FinalizableNativeFunction>(phv(func))->getContext())
      ->hostFunction;
}

jsi::Value HermesRuntimeImpl::call(
    const jsi::Function &func,
    const jsi::Value &jsThis,
    const jsi::Value *args,
    size_t count) {
  ::hermes::instrumentation::PerfMarker m("jsi-hermes-call");
  vm::GCScope gcScope(&runtime_);
  vm::Handle<vm::Callable> handle =
      vm::Handle<vm::Callable>::vmcast(&phv(func));
  if (count > std::numeric_limits<uint32_t>::max() ||
      !runtime_.checkAvailableStack((uint32_t)count)) {
    throw jsi::JSINativeException(
        "HermesRuntimeImpl::call: Unable to call function: stack overflow");
  }

  auto &stats = runtime_.getRuntimeStats();
  const vm::instrumentation::RAIITimer timer{
      "Incoming Function", stats, stats.incomingFunction};
  vm::ScopedNativeCallFrame newFrame{&runtime_,
                                     static_cast<uint32_t>(count),
                                     handle.getHermesValue(),
                                     vm::HermesValue::encodeUndefinedValue(),
                                     hvFromValue(jsThis)};
  if (LLVM_UNLIKELY(newFrame.overflowed())) {
    checkStatus(runtime_.raiseStackOverflow());
  }

  for (uint32_t i = 0; i != count; ++i) {
    newFrame->getArgRef(i) = hvFromValue(args[i]);
  }
  auto callRes = vm::Callable::call(handle, &runtime_);
  checkStatus(callRes.getStatus());

  return valueFromHermesValue(*callRes);
}

jsi::Value HermesRuntimeImpl::callAsConstructor(
    const jsi::Function &func,
    const jsi::Value *args,
    size_t count) {
  ::hermes::instrumentation::PerfMarker m("jsi-hermes-callAsConstructor");
  vm::GCScope gcScope(&runtime_);
  vm::Handle<vm::Callable> funcHandle =
      vm::Handle<vm::Callable>::vmcast(&phv(func));

  if (count > std::numeric_limits<uint32_t>::max() ||
      !runtime_.checkAvailableStack((uint32_t)count)) {
    throw jsi::JSINativeException(
        "HermesRuntimeImpl::call: Unable to call function: stack overflow");
  }

  auto &stats = runtime_.getRuntimeStats();
  const vm::instrumentation::RAIITimer timer{
      "Incoming Function: Call As Constructor", stats, stats.incomingFunction};

  // We follow es5 13.2.2 [[Construct]] here. Below F == func.
  // 13.2.2.5:
  //    Let proto be the value of calling the [[Get]] internal property of
  //    F with argument "prototype"
  auto protoRes = vm::JSObject::getNamed(
      funcHandle,
      &runtime_,
      runtime_.getPredefinedSymbolID(vm::Predefined::prototype));
  checkStatus(protoRes.getStatus());
  // 13.2.2.6:
  //    If Type(proto) is Object, set the [[Prototype]] internal property
  //    of obj to proto
  // 13.2.2.7:
  //    If Type(proto) is not Object, set the [[Prototype]] internal property
  //    of obj to the standard built-in Object prototype object as described in
  //    15.2.4
  //
  // Note that 13.2.2.1-4 are also handled by the call to newObject.
  auto protoValue = protoRes.getValue();
  auto protoHandle = protoValue.isObject()
      ? vm::Handle<vm::JSObject>::vmcast(&runtime_, protoValue)
      : vm::Handle<vm::JSObject>::vmcast(&runtime_.objectPrototype);
  auto thisRes = vm::Callable::newObject(funcHandle, &runtime_, protoHandle);
  checkStatus(thisRes.getStatus());
  // We need to capture this in case the ctor doesn't return an object,
  // we need to return this object.
  auto objHandle = runtime_.makeHandle<vm::JSObject>(*thisRes);

  // 13.2.2.8:
  //    Let result be the result of calling the [[Call]] internal property of
  //    F, providing obj as the this value and providing the argument list
  //    passed into [[Construct]] as args.
  //
  // For us result == res.

  vm::ScopedNativeCallFrame newFrame{&runtime_,
                                     static_cast<uint32_t>(count),
                                     funcHandle.getHermesValue(),
                                     funcHandle.getHermesValue(),
                                     objHandle.getHermesValue()};
  if (newFrame.overflowed()) {
    checkStatus(runtime_.raiseStackOverflow());
  }
  for (uint32_t i = 0; i != count; ++i) {
    newFrame->getArgRef(i) = hvFromValue(args[i]);
  }
  // The last parameter indicates that this call should construct an object.
  auto callRes = vm::Callable::call(funcHandle, &runtime_);
  checkStatus(callRes.getStatus());

  // 13.2.2.9:
  //    If Type(result) is Object then return result
  // 13.2.2.10:
  //    Return obj
  auto resultValue = *callRes;
  vm::HermesValue resultHValue =
      resultValue.isObject() ? resultValue : objHandle.getHermesValue();
  return valueFromHermesValue(resultHValue);
}

bool HermesRuntimeImpl::strictEquals(const jsi::String &a, const jsi::String &b)
    const {
  return phv(a).getString()->equals(phv(b).getString());
}

bool HermesRuntimeImpl::strictEquals(const jsi::Object &a, const jsi::Object &b)
    const {
  return phv(a).getRaw() == phv(b).getRaw();
}

bool HermesRuntimeImpl::instanceOf(
    const jsi::Object &o,
    const jsi::Function &f) {
  vm::GCScope gcScope(&runtime_);
  auto result = vm::instanceOfOperator(
      &runtime_, runtime_.makeHandle(phv(o)), runtime_.makeHandle(phv(f)));
  checkStatus(result.getStatus());
  return *result;
}

jsi::Runtime::ScopeState *HermesRuntimeImpl::pushScope() {
  hermesValues_->emplace_front(
      vm::HermesValue::encodeNativeValue(kSentinelNativeValue));
  return reinterpret_cast<ScopeState *>(&hermesValues_->front());
}

void HermesRuntimeImpl::popScope(ScopeState *prv) {
  HermesPointerValue *sentinel = reinterpret_cast<HermesPointerValue *>(prv);
  assert(sentinel->phv.isNativeValue());
  assert(sentinel->phv.getNativeValue() == kSentinelNativeValue);

  for (auto it = hermesValues_->begin(); it != hermesValues_->end();) {
    auto &value = *it;

    if (&value == sentinel) {
      hermesValues_->erase(it);
      return;
    }

    if (value.phv.isNativeValue()) {
      // We reached another sentinel value or we started added another native
      // value to the hermesValue_ list. This should not happen.
      std::terminate();
    }

    if (value.get() == 0) {
      it = hermesValues_->erase(it);
    } else {
      ++it;
    }
  }

  // We did not find a sentinel value.
  std::terminate();
}

void HermesRuntimeImpl::checkStatus(vm::ExecutionStatus status) {
  if (LLVM_UNLIKELY(status == vm::ExecutionStatus::EXCEPTION)) {
    jsi::Value exception = valueFromHermesValue(runtime_.getThrownValue());
    runtime_.clearThrownValue();
    throw jsi::JSError(*this, std::move(exception));
  }
}

vm::HermesValue HermesRuntimeImpl::stringHVFromUtf8(
    const uint8_t *utf8,
    size_t length) {
  std::u16string out;
  out.resize(length);
  const llvm::UTF8 *sourceStart = (const llvm::UTF8 *)utf8;
  const llvm::UTF8 *sourceEnd = sourceStart + length;
  llvm::UTF16 *targetStart = (llvm::UTF16 *)&out[0];
  llvm::UTF16 *targetEnd = targetStart + out.capacity();
  llvm::ConversionResult cRes;
  cRes = ConvertUTF8toUTF16(
      &sourceStart,
      sourceEnd,
      &targetStart,
      targetEnd,
      llvm::lenientConversion);
  (void)cRes;
  assert(
      cRes != llvm::ConversionResult::targetExhausted &&
      "not enough space allocated for UTF16 conversion");
  out.resize((char16_t *)targetStart - &out[0]);

  auto strRes = vm::StringPrimitive::create(&runtime_, out);
  checkStatus(strRes.getStatus());

  return *strRes;
}

size_t HermesRuntimeImpl::getLength(vm::Handle<vm::ArrayImpl> arr) {
  auto res = vm::JSObject::getNamed(
      arr, &runtime_, runtime_.getPredefinedSymbolID(vm::Predefined::length));
  checkStatus(res.getStatus());
  if (!res->isNumber()) {
    throw jsi::JSError(*this, "getLength: property 'length' is not a number");
  }

  return static_cast<size_t>(res->getDouble());
}

size_t HermesRuntimeImpl::getByteLength(vm::Handle<vm::JSArrayBuffer> arr) {
  auto res = vm::JSObject::getNamed(
      arr,
      &runtime_,
      runtime_.getPredefinedSymbolID(vm::Predefined::byteLength));
  checkStatus(res.getStatus());
  if (!res->isNumber()) {
    throw jsi::JSError(
        *this, "getLength: property 'byteLength' is not a number");
  }

  return static_cast<size_t>(res->getDouble());
}

template <typename Runtime>
void addRecordTTI(Runtime &rt) {
  rt.global().setProperty(
      rt,
      "nativeRecordTTI",
      jsi::Function::createFromHostFunction(
          rt,
          jsi::PropNameID::forAscii(rt, "nativeRecordTTI"),
          0,
          [](jsi::Runtime &rt, const jsi::Value &, const jsi::Value *, size_t)
              -> jsi::Value {
#if API_TRACE
            static_cast<TracingHermesRuntimeImpl &>(rt).addTTIMarker();
#endif
#if API_TRACE
            static_cast<TracingHermesRuntimeImpl &>(rt).ttiReached();
#else
            static_cast<HermesRuntimeImpl &>(rt).ttiReached();
#endif
#ifdef HERMESVM_PLATFORM_LOGGING
            logGCStats(rt, "TTI call");
#endif
            return jsi::Value::undefined();
          }));
}

std::unique_ptr<HermesRuntime> makeHermesRuntime(
    const vm::RuntimeConfig &runtimeConfig,
    bool shouldExposeTraceFunctions) {
  // This is insurance against someone adding data members to
  // HermesRuntime.  If on some weird platform it fails, it can be
  // updated or removed.
  static_assert(
      sizeof(HermesRuntime) == sizeof(void *),
      "HermesRuntime should only include a vtable ptr");

#if API_TRACE
  auto ret = std::make_unique<TracingHermesRuntimeImpl>(runtimeConfig);
#elif defined(HERMESVM_PLATFORM_LOGGING)
  auto ret = std::make_unique<HermesRuntimeImpl>(
      runtimeConfig.rebuild()
          .withGCConfig(runtimeConfig.getGCConfig()
                            .rebuild()
                            .withShouldRecordStats(true)
                            .build())
          .build());
#else
  auto ret = std::make_unique<HermesRuntimeImpl>(runtimeConfig);
#endif

  if (shouldExposeTraceFunctions) {
    addRecordTTI(*ret);
  }

#ifdef HERMES_ENABLE_DEBUGGER
  // Only HermesRuntime can create a debugger instance.  This requires
  // the setter and not using make_unique, so the call to new is here
  // in this function, which is a friend of debugger::Debugger.
  ret->setDebugger(std::unique_ptr<debugger::Debugger>(
      new debugger::Debugger(ret.get(), &(ret->runtime_.getDebugger()))));
#endif

  return std::move(ret);
}

std::unique_ptr<jsi::ThreadSafeRuntime> makeThreadSafeHermesRuntime(
    const vm::RuntimeConfig &runtimeConfig,
    bool shouldExposeTraceFunctions) {
#if API_TRACE
  auto ret = std::make_unique<
      jsi::detail::ThreadSafeRuntimeImpl<TracingHermesRuntimeImpl, Lock>>(
      runtimeConfig);
#elif defined(HERMESVM_PLATFORM_LOGGING)
  auto ret = std::make_unique<
      jsi::detail::ThreadSafeRuntimeImpl<HermesRuntimeImpl, Lock>>(
      runtimeConfig.rebuild()
          .withGCConfig(runtimeConfig.getGCConfig()
                            .rebuild()
                            .withShouldRecordStats(true)
                            .build())
          .build());
#else
  auto ret = std::make_unique<
      jsi::detail::ThreadSafeRuntimeImpl<HermesRuntimeImpl, Lock>>(
      runtimeConfig);
#endif

  if (shouldExposeTraceFunctions) {
    addRecordTTI(*ret);
  }

#ifdef HERMES_ENABLE_DEBUGGER
  auto &hermesRt = ret->getUnsafeRuntime();
  // Only HermesRuntime can create a debugger instance.  This requires
  // the setter and not using make_unique, so the call to new is here
  // in this function, which is a friend of debugger::Debugger.
  hermesRt.setDebugger(std::unique_ptr<debugger::Debugger>(
      new debugger::Debugger(&hermesRt, &(hermesRt.runtime_.getDebugger()))));
#endif

  return std::move(ret);
}

#ifdef HERMES_ENABLE_DEBUGGER
/// Glue code enabling the Debugger to produce a jsi::Value from a HermesValue.
jsi::Value debugger::Debugger::jsiValueFromHermesValue(vm::HermesValue hv) {
  return impl(runtime_)->valueFromHermesValue(hv);
}
#endif

} // namespace hermes
} // namespace facebook
