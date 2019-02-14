/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the LICENSE
 * file in the root directory of this source tree.
 */
#define DEBUG_TYPE "vm"
#include "hermes/VM/Runtime.h"

#include "hermes/AST/SemValidate.h"
#include "hermes/BCGen/HBC/Bytecode.h"
#include "hermes/BCGen/HBC/BytecodeGenerator.h"
#include "hermes/BCGen/HBC/HBC.h"
#include "hermes/BCGen/HBC/SimpleBytecodeBuilder.h"
#include "hermes/IR/IR.h"
#include "hermes/IRGen/IRGen.h"
#include "hermes/Inst/Builtins.h"
#include "hermes/Parser/JSParser.h"
#include "hermes/Runtime/Libhermes.h"
#include "hermes/Support/CheckedMalloc.h"
#include "hermes/Support/PerfSection.h"
#include "hermes/VM/BuildMetadata.h"
#include "hermes/VM/Callable.h"
#include "hermes/VM/CodeBlock.h"
#include "hermes/VM/IdentifierTable.h"
#include "hermes/VM/JSError.h"
#include "hermes/VM/JSLib.h"
#include "hermes/VM/JSLib/RuntimeCommonStorage.h"
#include "hermes/VM/Operations.h"
#include "hermes/VM/Profiler/SamplingProfiler.h"
#include "hermes/VM/StackFrame-inline.h"
#include "hermes/VM/StringView.h"

#ifndef HERMESVM_LEAN
#include "hermes/Support/MemoryBuffer.h"
#endif

#include "llvm/ADT/Hashing.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

namespace hermes {
namespace vm {

namespace {

static const Predefined fixedPropCacheNames[(size_t)PropCacheID::_COUNT] = {
#define V(id, predef) predef,
    PROP_CACHE_IDS(V)
#undef V
};

} // namespace

/* static */
std::shared_ptr<Runtime> Runtime::create(const RuntimeConfig &runtimeConfig) {
  const GCConfig &gcConfig = runtimeConfig.getGCConfig();
  GC::Size sz{gcConfig.getMinHeapSize(), gcConfig.getMaxHeapSize()};
  std::shared_ptr<StorageProvider> provider{
#ifdef HERMESVM_FLAT_ADDRESS_SPACE
      StorageProvider::defaultProviderWithExcess(
          sz.storageFootprint(), sizeof(Runtime))
#else
      StorageProvider::defaultProvider(sz.storageFootprint())
#endif
  };
  Runtime *rt = nullptr;
#ifdef HERMESVM_FLAT_ADDRESS_SPACE
  // Place Runtime in the first allocated storage.
  void *storage = provider->newStorage();
  if (LLVM_UNLIKELY(!storage)) {
    hermes_fatal("Could not allocate initial storage for Runtime");
  }
  rt = new (storage) Runtime(provider.get(), runtimeConfig);
#else
  // When not using the flat address space, allocate runtime normally.
  rt = new Runtime(provider.get(), runtimeConfig);
#endif
  // Return a shared pointer with a custom deleter to delete the underlying
  // storage of the runtime.
  return std::shared_ptr<Runtime>{rt, [provider](Runtime *runtime) {
#ifdef HERMESVM_FLAT_ADDRESS_SPACE
                                    runtime->~Runtime();
                                    provider->deleteStorage(runtime);
#else
                                    delete runtime;
                                    // Provider is only captured to keep it
                                    // alive until after the Runtime is deleted.
                                    (void)provider;
#endif
                                  }};
}

CallResult<HermesValue> Runtime::getNamed(
    Handle<JSObject> obj,
    PropCacheID id) {
  auto *clazz = obj->getClass();
  auto *cacheEntry = &fixedPropCache_[static_cast<int>(id)];
  if (LLVM_LIKELY(cacheEntry->clazz == clazz)) {
    return JSObject::getNamedSlotValue<PropStorage::Inline::Yes>(
        *obj, cacheEntry->slot);
  }
  auto sym = getPredefinedSymbolID(fixedPropCacheNames[static_cast<int>(id)]);
  NamedPropertyDescriptor desc;
  // Check writable and internalSetter flags since the cache slot is shared for
  // get/put.
  if (LLVM_LIKELY(JSObject::tryGetOwnNamedDescriptorFast(*obj, sym, desc)) &&
      !desc.flags.accessor && desc.flags.writable &&
      !desc.flags.internalSetter) {
    if (LLVM_LIKELY(!clazz->isDictionary())) {
      // Cache the class, id and property slot.
      cacheEntry->clazz = clazz;
      cacheEntry->slot = desc.slot;
    }
    return JSObject::getNamedSlotValue(*obj, desc);
  }
  return JSObject::getNamed(obj, this, sym);
}

ExecutionStatus Runtime::putNamedThrowOnError(
    Handle<JSObject> obj,
    PropCacheID id,
    HermesValue hv) {
  auto *clazz = obj->getClass();
  auto *cacheEntry = &fixedPropCache_[static_cast<int>(id)];
  if (LLVM_LIKELY(cacheEntry->clazz == clazz)) {
    JSObject::setNamedSlotValue<PropStorage::Inline::Yes>(
        *obj, this, cacheEntry->slot, hv);
    return ExecutionStatus::RETURNED;
  }
  auto sym = getPredefinedSymbolID(fixedPropCacheNames[static_cast<int>(id)]);
  NamedPropertyDescriptor desc;
  if (LLVM_LIKELY(JSObject::tryGetOwnNamedDescriptorFast(*obj, sym, desc)) &&
      !desc.flags.accessor && desc.flags.writable &&
      !desc.flags.internalSetter) {
    if (LLVM_LIKELY(!clazz->isDictionary())) {
      // Cache the class and property slot.
      cacheEntry->clazz = clazz;
      cacheEntry->slot = desc.slot;
    }
    JSObject::setNamedSlotValue(*obj, this, desc.slot, hv);
    return ExecutionStatus::RETURNED;
  }
  return JSObject::putNamed(
             obj, this, sym, makeHandle(hv), PropOpFlags().plusThrowOnError())
      .getStatus();
}

Runtime::Runtime(StorageProvider *provider, const RuntimeConfig &runtimeConfig)
    // The initial heap size can't be larger than the max.
    : enableEval(runtimeConfig.getEnableEval()),
      heap_(getMetadataTable(), this, runtimeConfig.getGCConfig(), provider),
      jitContext_(runtimeConfig.getEnableJIT(), (1 << 20) * 8, (1 << 20) * 32),
      hasES6Symbol_(runtimeConfig.getES6Symbol()),
      shouldRandomizeMemoryLayout_(runtimeConfig.getRandomizeMemoryLayout()),
      bytecodeWarmupPercent_(runtimeConfig.getBytecodeWarmupPercent()),
      runtimeStats_(runtimeConfig.getEnableSampledStats()),
      commonStorage_(createRuntimeCommonStorage()),
      stackPointer_() {
  assert(
      (void *)this == (void *)(HandleRootOwner *)this &&
      "cast to HandleRootOwner should be no-op");
  auto maxNumRegisters = runtimeConfig.getMaxNumRegisters();
  registerStack_ = runtimeConfig.getRegisterStack();
  if (!registerStack_) {
    // registerStack_ should be allocated with malloc instead of new so that the
    // default constructors don't run for the whole stack space.
    registerStack_ = static_cast<PinnedHermesValue *>(
        checkedMalloc(sizeof(PinnedHermesValue) * maxNumRegisters));
  } else {
    freeRegisterStack_ = false;
  }

  registerStackEnd_ = registerStack_ + maxNumRegisters;
  if (shouldRandomizeMemoryLayout_) {
    const unsigned bytesOff = std::random_device()() % oscompat::page_size();
    registerStackEnd_ -= bytesOff / sizeof(PinnedHermesValue);
    assert(registerStackEnd_ >= registerStack_ && "register stack too small");
  }
  stackPointer_ = registerStackEnd_;

  // Setup the "root" stack frame.
  setCurrentFrameToTopOfStack();
  // Allocate the "reserved" registers in the root frame.
  allocStack(
      StackFrameLayout::CalleeExtraRegistersAtStart,
      HermesValue::encodeUndefinedValue());

  // Initialize special code blocks pointing to their own runtime module.
  // specialCodeBlockRuntimeModule_ will be owned by runtimeModuleList_.
  RuntimeModuleFlags flags;
  flags.hidesEpilogue = true;
  specialCodeBlockRuntimeModule_ = RuntimeModule::createManual(
      this,
      hbc::BCProviderFromBuffer::createBCProviderFromBuffer(
          generateSpecialRuntimeBytecode())
          .first,
      flags);
  assert(
      &runtimeModuleList_.back() == specialCodeBlockRuntimeModule_ &&
      "specialCodeBlockRuntimeModule_ not added to runtimeModuleList_");
  emptyCodeBlock_ = specialCodeBlockRuntimeModule_->getCodeBlock(0);
  returnThisCodeBlock_ = specialCodeBlockRuntimeModule_->getCodeBlock(1);

  // Initialize predefinedStrings_.
  // This function does not do any allocations.
  initPredefinedStrings();

  // At this point, allocations can begin, as all the roots are markable.

  // Initialize the pre-allocated character strings.
  initCharacterStrings();

  GCScope scope(this);

  // Initialize the root hidden class.
  rootClazz_ = ignoreAllocationFailure(HiddenClass::createRoot(this));
  rootClazzRawPtr_ = vmcast<HiddenClass>(rootClazz_);

  // Initialize the global object.

  global_ =
      JSObject::create(this, Handle<JSObject>(this, nullptr)).getHermesValue();

  initGlobalObject(this);

  // Once the global object has been initialized, populate the builtins table.
  initBuiltinTable();

  stringCycleCheckVisited_ =
      ignoreAllocationFailure(ArrayStorage::create(this, 8));

  // Set the prototype of the global object to the standard object prototype,
  // which has now been defined.
  ignoreAllocationFailure(JSObject::setParent(
      vmcast<JSObject>(global_), this, vmcast<JSObject>(objectPrototype)));

  symbolRegistry_.init(this);

  DEBUG(llvm::dbgs() << "Runtime initialized\n");

  samplingProfiler_ = SamplingProfiler::getInstance();
  samplingProfiler_->registerRuntime(this);
}

Runtime::~Runtime() {
  samplingProfiler_->unregisterRuntime(this);

  heap_.finalizeAll();
  if (freeRegisterStack_) {
    ::free(registerStack_);
  }
  // Remove inter-module dependencies so we can delete them in any order.
  for (auto &module : runtimeModuleList_) {
    module.prepareForRuntimeShutdown();
  }

  while (!runtimeModuleList_.empty()) {
    // Calling delete will automatically remove it from the list.
    delete &runtimeModuleList_.back();
  }
}

/// A helper class used to measure the duration of GC marking different roots.
/// It accumulates the times in \c Runtime::markRootsPhaseTimes[] and \c
/// Runtime::totalMarkRootsTime.
class Runtime::MarkRootsPhaseTimer {
 public:
  MarkRootsPhaseTimer(Runtime *rt, Runtime::MarkRootsPhase phase)
      : rt_(rt), phase_(phase), start_(std::chrono::steady_clock::now()) {
    if (static_cast<unsigned>(phase) == 0) {
      // The first phase; record the start as the start of markRoots.
      rt_->startOfMarkRoots_ = start_;
    }
  }
  ~MarkRootsPhaseTimer() {
    auto tp = std::chrono::steady_clock::now();
    std::chrono::duration<double> elapsed = (tp - start_);
    start_ = tp;
    unsigned index = static_cast<unsigned>(phase_);
    rt_->markRootsPhaseTimes_[index] += elapsed.count();
    if (index + 1 ==
        static_cast<unsigned>(Runtime::MarkRootsPhase::NumPhases)) {
      std::chrono::duration<double> totalElapsed =
          (tp - rt_->startOfMarkRoots_);
      rt_->totalMarkRootsTime_ += totalElapsed.count();
    }
  }

 private:
  Runtime *rt_;
  MarkRootsPhase phase_;
  std::chrono::time_point<std::chrono::steady_clock> start_;
};

void Runtime::markRoots(
    GC *gc,
    SlotAcceptorWithNames &acceptor,
    bool markLongLived) {
  // The body of markRoots should be sequence of blocks, each of which
  // starts with the declaration of an appropriate MarkRootsPhase
  // instance.
  {
    MarkRootsPhaseTimer timer(this, MarkRootsPhase::Registers);
    for (auto *p = stackPointer_, *e = registerStackEnd_; p != e; ++p)
      acceptor.accept(*p);
  }

  {
    MarkRootsPhaseTimer timer(this, MarkRootsPhase::RuntimeInstanceVars);
    acceptor.accept(thrownValue_, "@thrownValue");
    acceptor.accept(nullPointer_, "@nullPointer");
    acceptor.accept(rootClazz_, "@rootClass");
    acceptor.acceptPtr(rootClazzRawPtr_, "@rootClass");
    acceptor.accept(stringCycleCheckVisited_, "@stringCycleCheckVisited");
    acceptor.accept(global_, "@global");
#ifdef HERMES_ENABLE_DEBUGGER
    acceptor.accept(debuggerInternalObject_, "@debuggerInternal");
#endif // HERMES_ENABLE_DEBUGGER
  }

  {
    MarkRootsPhaseTimer timer(this, MarkRootsPhase::RuntimeModules);
    for (auto &rm : runtimeModuleList_)
      rm.markRoots(acceptor, markLongLived);
    for (auto &entry : fixedPropCache_) {
      acceptor.acceptPtr(entry.clazz);
    }
  }

  {
    MarkRootsPhaseTimer timer(this, MarkRootsPhase::CharStrings);
    if (markLongLived) {
      for (auto &hv : charStrings_)
        acceptor.accept(hv);
    }
  }

  {
    MarkRootsPhaseTimer timer(this, MarkRootsPhase::Builtins);
    for (NativeFunction *&nf : builtins_)
      acceptor.accept((void *&)nf);
  }

#ifdef MARK
#error "Shouldn't have defined mark already"
#endif
#define MARK(field) acceptor.accept((field), "@" #field)
  {
    MarkRootsPhaseTimer timer(this, MarkRootsPhase::Prototypes);
    // Prototypes.
    MARK(objectPrototype);
    acceptor.acceptPtr(objectPrototypeRawPtr, "@objectPrototype");
    MARK(functionPrototype);
    acceptor.acceptPtr(functionPrototypeRawPtr, "@functionPrototype");
    MARK(stringPrototype);
    MARK(numberPrototype);
    MARK(booleanPrototype);
    MARK(symbolPrototype);
    MARK(datePrototype);
    MARK(arrayPrototype);
    acceptor.acceptPtr(arrayPrototypeRawPtr, "@arrayPrototype");
    MARK(arrayBufferPrototype);
    MARK(dataViewPrototype);
    MARK(typedArrayBasePrototype);
    MARK(setPrototype);
    MARK(setIteratorPrototype);
    MARK(mapPrototype);
    MARK(mapIteratorPrototype);
    MARK(weakMapPrototype);
    MARK(weakSetPrototype);
    MARK(regExpPrototype);
    // Constructors.
    MARK(typedArrayBaseConstructor);
    // Miscellaneous.
    MARK(regExpLastInput);
    MARK(regExpLastRegExp);
    MARK(throwTypeErrorAccessor);
    MARK(throwInvalidRequire);
    MARK(arrayClass);
    acceptor.acceptPtr(arrayClassRawPtr, "@arrayClass");
    MARK(iteratorPrototype);
    MARK(arrayIteratorPrototype);
    MARK(arrayPrototypeValues);
    MARK(stringIteratorPrototype);
    MARK(jsErrorStackAccessor);
    MARK(parseIntFunction);
    MARK(parseFloatFunction);
    MARK(requireFunction);

#define TYPED_ARRAY(name, type)                                      \
  acceptor.accept(name##ArrayPrototype, "@" #name "ArrayPrototype"); \
  acceptor.accept(name##ArrayConstructor, "@" #name "ArrayConstructor");
#include "hermes/VM/TypedArrays.def"

    MARK(errorConstructor);
#define ALL_ERROR_TYPE(name) \
  acceptor.accept(name##Prototype, "@" #name "Prototype");
#include "hermes/VM/NativeErrorTypes.def"
#undef MARK
  }

  {
    MarkRootsPhaseTimer timer(this, MarkRootsPhase::IdentifierTable);
    if (markLongLived) {
      identifierTable_.markIdentifiers(acceptor, &getHeap());
    }
  }

  {
    MarkRootsPhaseTimer timer(this, MarkRootsPhase::GCScopes);
    markGCScopes(acceptor);
  }

  {
    MarkRootsPhaseTimer timer(this, MarkRootsPhase::WeakRefs);
    markWeakRefs(gc);
  }

  {
    MarkRootsPhaseTimer timer(this, MarkRootsPhase::SymbolRegistry);
    symbolRegistry_.markRoots(acceptor);
  }

  {
    MarkRootsPhaseTimer timer(this, MarkRootsPhase::Custom);
    for (auto &fn : customMarkRootFuncs_)
      fn(gc, acceptor);
  }
}

void Runtime::markWeakRoots(GCBase *gc, SlotAcceptorWithNames &acceptor) {
  for (auto &rm : runtimeModuleList_)
    rm.markWeakRoots(acceptor);
}

void Runtime::visitIdentifiers(
    const std::function<void(UTF16Ref, uint32_t)> &acceptor) {
  identifierTable_.visitIdentifiers(acceptor);
}

void Runtime::printRuntimeGCStats(llvm::raw_ostream &os) const {
  const unsigned kNumPhases = static_cast<unsigned>(MarkRootsPhase::NumPhases);
#define MARK_ROOTS_PHASE(phase) "MarkRoots_" #phase,
  static const char *markRootsPhaseNames[kNumPhases] = {
#include "hermes/VM/MarkRootsPhases.def"
  };
#undef MARK_ROOTS_PHASE
  os << "\t\"runtime\": {\n";
  os << "\t\t\"totalMarkRootsTime\": " << formatSecs(totalMarkRootsTime_).secs
     << ",\n";
  bool first = true;
  for (unsigned phaseNum = 0; phaseNum < kNumPhases; phaseNum++) {
    if (first) {
      first = false;
    } else {
      os << ",\n";
    }
    os << "\t\t\"" << markRootsPhaseNames[phaseNum] << "Time"
       << "\": " << formatSecs(markRootsPhaseTimes_[phaseNum]).secs;
  }
  os << "\n\t}";
}

unsigned Runtime::getSymbolsEnd() const {
  return identifierTable_.getSymbolsEnd();
}

void Runtime::freeSymbols(const std::vector<bool> &markedSymbols) {
  identifierTable_.freeUnmarkedSymbols(markedSymbols);
}

size_t Runtime::mallocSize() const {
  size_t totalSize = 0;

  // Capacity of the register stack.
  totalSize += sizeof(*registerStack_) * (registerStackEnd_ - registerStack_);
  // IdentifierTable size
  totalSize +=
      sizeof(IdentifierTable) + identifierTable_.additionalMemorySize();
  // Runtime modules
  for (const RuntimeModule &rtm : runtimeModuleList_) {
    totalSize += sizeof(RuntimeModule) + rtm.additionalMemorySize();
  }
  return totalSize;
}

void Runtime::setMockedEnvironment(const MockedEnvironment &env) {
#ifdef HERMESVM_SYNTH_REPLAY
  getCommonStorage()->env = env;
#endif
}

LLVM_ATTRIBUTE_NOINLINE
static CallResult<HermesValue> interpretFunctionWithRandomStack(
    Runtime *runtime,
    CodeBlock *globalCode) {
  static void *volatile dummy;
  const unsigned amount = std::random_device()() % oscompat::page_size();
  // Prevent compiler from optimizing alloca away by assigning to volatile
  dummy = alloca(amount);
  (void)dummy;
  return runtime->interpretFunction(globalCode);
}

ExecutionStatus Runtime::run(
    llvm::StringRef code,
    llvm::StringRef sourceURL,
    const hbc::CompileFlags &compileFlags) {
#ifdef HERMESVM_LEAN
  return raiseEvalUnsupported(code);
#else
  std::unique_ptr<hermes::Buffer> buffer;
  if (compileFlags.lazy) {
    buffer.reset(new hermes::OwnedMemoryBuffer(
        llvm::MemoryBuffer::getMemBufferCopy(code)));
  } else {
    buffer.reset(
        new hermes::OwnedMemoryBuffer(llvm::MemoryBuffer::getMemBuffer(code)));
  }
  return run(std::move(buffer), sourceURL, compileFlags);
#endif
}

ExecutionStatus Runtime::run(
    std::unique_ptr<hermes::Buffer> code,
    llvm::StringRef sourceURL,
    const hbc::CompileFlags &compileFlags) {
#ifdef HERMESVM_LEAN
  auto buffer = code.get();
  return raiseEvalUnsupported(llvm::StringRef(
      reinterpret_cast<const char *>(buffer->data()), buffer->size()));
#else

  std::unique_ptr<hbc::BCProviderFromSrc> bytecode;
  {
    PerfSection loading("Loading new JavaScript code");
    loading.addArg("url", sourceURL);
    auto bytecode_err = hbc::BCProviderFromSrc::createBCProviderFromSrc(
        std::move(code), sourceURL, compileFlags);
    if (!bytecode_err.first) {
      return raiseSyntaxError(TwineChar16(bytecode_err.second));
    }
    bytecode = std::move(bytecode_err.first);
  }

  PerfSection loading("Executing global function");
  RuntimeModuleFlags rmflags;
  rmflags.persistent = true;
  return runBytecode(
      std::move(bytecode), rmflags, sourceURL, makeNullHandle<Environment>());
#endif
}

CallResult<HermesValue> Runtime::runBytecode(
    std::shared_ptr<hbc::BCProvider> &&bytecode,
    RuntimeModuleFlags flags,
    llvm::StringRef sourceURL,
    Handle<Environment> environment,
    Handle<> thisArg) {
  clearThrownValue();

  auto globalFunctionIndex = bytecode->getGlobalFunctionIndex();

  if (bytecode->getBytecodeOptions().staticBuiltins && !builtinsFrozen_) {
    if (assertBuiltinsUnmodified() == ExecutionStatus::EXCEPTION) {
      return ExecutionStatus::EXCEPTION;
    }
    freezeBuiltins();
    assert(builtinsFrozen_ && "Builtins must be frozen by now.");
  }

  if (flags.persistent && bytecodeWarmupPercent_ > 0) {
    // Start the warmup thread for this bytecode if it's a buffer.
    bytecode->startWarmup(bytecodeWarmupPercent_);
  }
  auto runtimeModule =
      RuntimeModule::create(this, std::move(bytecode), flags, sourceURL);
  auto globalCode = runtimeModule->getCodeBlock(globalFunctionIndex);

  GCScope scope(this);

#ifdef HERMES_ENABLE_DEBUGGER
  // If the debugger is configured to pause on load, give it a chance to pause.
  getDebugger().willExecuteModule(runtimeModule, globalCode);
#endif

  // Create a JSFunction which will reference count the runtime module.
  // Note that its handle gets registered in the scope, so we don't need to
  // save it. Also note that environment will often be null here, except if this
  // is local eval.
  auto funcRes = JSFunction::create(
      this,
      Handle<JSObject>::vmcast(&functionPrototype),
      environment,
      globalCode);

  if (funcRes == ExecutionStatus::EXCEPTION)
    return ExecutionStatus::EXCEPTION;

  ScopedNativeCallFrame newFrame{
      this, 0, *funcRes, HermesValue::encodeUndefinedValue(), *thisArg};
  if (LLVM_UNLIKELY(newFrame.overflowed()))
    return raiseStackOverflow();
  return shouldRandomizeMemoryLayout_
      ? interpretFunctionWithRandomStack(this, globalCode)
      : interpretFunction(globalCode);
}

void Runtime::printException(llvm::raw_ostream &os, Handle<> valueHandle) {
  clearThrownValue();

  // Try to fetch the stack trace.
  CallResult<HermesValue> propRes{ExecutionStatus::EXCEPTION};
  if (auto objHandle = Handle<JSObject>::dyn_vmcast(this, valueHandle)) {
    if (LLVM_UNLIKELY(
            (propRes = JSObject::getNamed(
                 objHandle, this, getPredefinedSymbolID(Predefined::stack))) ==
            ExecutionStatus::EXCEPTION)) {
      os << "exception thrown while getting stack trace\n";
      return;
    }
  }
  SmallU16String<32> tmp;
  if (LLVM_UNLIKELY(
          propRes == ExecutionStatus::EXCEPTION || propRes->isUndefined())) {
    // If stack trace is unavailable, we just print error.toString.
    auto strRes = toString(this, valueHandle);
    if (LLVM_UNLIKELY(strRes == ExecutionStatus::EXCEPTION)) {
      os << "exception thrown in toString of original exception\n";
      return;
    }

    strRes->get()->copyUTF16String(tmp);
    os << tmp << "\n";
    return;
  }
  // stack trace is available, try to convert it to string.
  auto strRes = toString(this, makeHandle(*propRes));
  if (LLVM_UNLIKELY(strRes == ExecutionStatus::EXCEPTION)) {
    os << "exception thrown in toString of stack trace\n";
    return;
  }
  PseudoHandle<StringPrimitive> str = std::move(*strRes);
  if (str->getStringLength() == 0) {
    str.invalidate();
    // If the final value is the empty string,
    // fall back to just printing the error.toString directly.
    auto errToStringRes = toString(this, valueHandle);
    if (LLVM_UNLIKELY(errToStringRes == ExecutionStatus::EXCEPTION)) {
      os << "exception thrown in toString of original exception\n";
      return;
    }
    str = std::move(*errToStringRes);
  }
  str->copyUTF16String(tmp);
  os << tmp << "\n";
}

Handle<HiddenClass> Runtime::getHiddenClassForPrototype(
    Handle<JSObject> proto) {
  return Handle<HiddenClass>::vmcast(&rootClazz_);
}

Handle<HiddenClass> Runtime::getHiddenClassForPrototype(
    PinnedHermesValue *proto) {
  return getHiddenClassForPrototype(Handle<JSObject>::vmcast(proto));
}

Handle<JSObject> Runtime::getGlobal() {
  return Handle<JSObject>::vmcast(&global_);
}

std::vector<llvm::ArrayRef<uint8_t>> Runtime::getEpilogues() {
  std::vector<llvm::ArrayRef<uint8_t>> result;
  for (const auto &m : runtimeModuleList_) {
    if (!m.hidesEpilogue()) {
      result.push_back(m.getEpilogue());
    }
  }
  return result;
}

#ifdef HERMES_ENABLE_DEBUGGER

llvm::Optional<Runtime::StackFrameInfo> Runtime::stackFrameInfoByIndex(
    uint32_t frameIdx) const {
  // Locate the frame.
  auto frames = getStackFrames();
  auto it = frames.begin();
  for (; frameIdx && it != frames.end(); ++it, --frameIdx) {
  }
  if (it == frames.end())
    return llvm::None;

  StackFrameInfo info;
  info.frame = *it++;
  info.isGlobal = it == frames.end();
  return info;
}

/// Calculate and \return the offset between the location of the specified
/// frame and the start of the stack. This value increases with every nested
/// call.
uint32_t Runtime::calcFrameOffset(ConstStackFrameIterator it) const {
  assert(it != getStackFrames().end() && "invalid frame");
  return registerStackEnd_ - it->ptr();
}

/// \return the offset between the location of the current frame and the
///   start of the stack. This value increases with every nested call.
uint32_t Runtime::getCurrentFrameOffset() const {
  return calcFrameOffset(getStackFrames().begin());
}

#endif

/// A placeholder used to construct a Error Object that takes in a const
/// message. A new StringPrimitive is created each time.
// TODO: Predefine each error message.
static ExecutionStatus raisePlaceholder(
    Runtime *runtime,
    Handle<JSObject> prototype,
    const TwineChar16 &msg) {
  // Since this happens unexpetedly and rarely, don't rely on the parent
  // GCScope.
  GCScope gcScope{runtime};

  SmallU16String<64> buf;
  msg.toVector(buf);

  auto strRes = StringPrimitive::create(runtime, buf);
  if (strRes == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  auto str = runtime->makeHandle<StringPrimitive>(*strRes);
  DEBUG(llvm::errs() << buf.arrayRef() << "\n");
  // Create the error object, initialize stack property and set message.
  auto errRes = JSError::create(runtime, prototype);
  if (LLVM_UNLIKELY(errRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  auto errorObj = runtime->makeHandle<JSError>(*errRes);
  JSError::recordStackTrace(errorObj, runtime);
  JSError::setupStack(errorObj, runtime);
  JSError::setMessage(errorObj, runtime, str);
  return runtime->setThrownValue(errorObj.getHermesValue());
}

ExecutionStatus Runtime::raiseTypeErrorForValue(
    Handle<> value,
    llvm::StringRef msg) {
  switch (value->getTag()) {
    case ObjectTag:
      return raiseTypeError(TwineChar16("Object") + msg);
    case StrTag:
      return raiseTypeError(
          TwineChar16("\"") + vmcast<StringPrimitive>(*value) + "\"" + msg);
    case BoolTag:
      if (value->getBool()) {
        return raiseTypeError(TwineChar16("true") + msg);
      } else {
        return raiseTypeError(TwineChar16("false") + msg);
      }
    case NullTag:
      return raiseTypeError(TwineChar16("null") + msg);
    case UndefinedTag:
      return raiseTypeError(TwineChar16("undefined") + msg);
    default:
      if (value->isNumber()) {
        char buf[hermes::NUMBER_TO_STRING_BUF_SIZE];
        size_t len = numberToString(
            value->getNumber(), buf, hermes::NUMBER_TO_STRING_BUF_SIZE);
        return raiseTypeError(TwineChar16(llvm::StringRef{buf, len}) + msg);
      }
      return raiseTypeError(TwineChar16("Value") + msg);
  }
}

ExecutionStatus Runtime::raiseTypeError(const TwineChar16 &msg) {
  return raisePlaceholder(
      this, Handle<JSObject>::vmcast(&TypeErrorPrototype), msg);
}

ExecutionStatus Runtime::raiseSyntaxError(const TwineChar16 &msg) {
  return raisePlaceholder(
      this, Handle<JSObject>::vmcast(&SyntaxErrorPrototype), msg);
}

ExecutionStatus Runtime::raiseRangeError(const TwineChar16 &msg) {
  return raisePlaceholder(
      this, Handle<JSObject>::vmcast(&RangeErrorPrototype), msg);
}

ExecutionStatus Runtime::raiseReferenceError(const TwineChar16 &msg) {
  return raisePlaceholder(
      this, Handle<JSObject>::vmcast(&ReferenceErrorPrototype), msg);
}

ExecutionStatus Runtime::raiseURIError(const TwineChar16 &msg) {
  return raisePlaceholder(
      this, Handle<JSObject>::vmcast(&URIErrorPrototype), msg);
}

ExecutionStatus Runtime::raiseStackOverflow() {
  return raisePlaceholder(
      this,
      Handle<JSObject>::vmcast(&RangeErrorPrototype),
      "Maximum call stack size exceeded");
}

ExecutionStatus Runtime::raiseEvalUnsupported(llvm::StringRef code) {
  return raiseSyntaxError(
      TwineChar16("Parsing source code unsupported: ") + code.substr(0, 32));
}

CallResult<bool> Runtime::insertVisitedObject(Handle<JSObject> obj) {
  bool foundCycle = false;
  MutableHandle<ArrayStorage> stack{
      this, vmcast<ArrayStorage>(stringCycleCheckVisited_)};
  for (uint32_t i = 0, len = stack->size(); i < len; ++i) {
    if (stack->at(i).getObject() == obj.get()) {
      foundCycle = true;
      break;
    }
  }
  if (ArrayStorage::push_back(stack, this, obj) == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  stringCycleCheckVisited_ = stack.getHermesValue();
  return foundCycle;
}

void Runtime::removeVisitedObject(Handle<JSObject> obj) {
  (void)obj;
  auto stack = Handle<ArrayStorage>::vmcast(&stringCycleCheckVisited_);
  auto elem = stack->pop_back();
  (void)elem;
  assert(
      elem.isObject() && elem.getObject() == obj.get() &&
      "string cycle check: stack corrupted");
}

std::unique_ptr<Buffer> Runtime::generateSpecialRuntimeBytecode() {
  hbc::SimpleBytecodeBuilder builder;
  {
    hbc::BytecodeInstructionGenerator bcGen;
    bcGen.emitLoadConstUndefined(0);
    bcGen.emitRet(0);
    builder.addFunction(1, bcGen.acquireBytecode());
  }
  {
    hbc::BytecodeInstructionGenerator bcGen;
    bcGen.emitGetGlobalObject(0);
    bcGen.emitRet(0);
    builder.addFunction(1, bcGen.acquireBytecode());
  }
  return builder.generateBytecodeBuffer();
}

void Runtime::initPredefinedStrings() {
  assert(!getTopGCScope() && "There shouldn't be any handles allocated yet");
  predefinedStrings_.resize((unsigned)Predefined::_PREDEFINED_COUNT);
  /// Create a buffer containing all strings.
  /// This ensures that all the strings live together in memory,
  /// and that we don't touch multiple separate pages to run this.
  static const char buffer[] =
#define STR(name, string) string
#include "hermes/VM/PredefinedStrings.def"
#define SYM(name, desc) desc
#include "hermes/VM/PredefinedStrings.def"
      ;
  static const uint8_t strLengths[] = {
#define STR(name, string) sizeof(string) - 1,
#include "hermes/VM/PredefinedStrings.def"
  };
  static const uint8_t symLengths[] = {
#define SYM(name, desc) sizeof(desc) - 1,
#include "hermes/VM/PredefinedStrings.def"
  };
  static const uint32_t hashes[] = {
#define STR(name, string) constexprHashString(string),
#include "hermes/VM/PredefinedStrings.def"
  };
  constexpr uint32_t strCount = sizeof strLengths / sizeof strLengths[0];
  static_assert(
      strCount == sizeof hashes / sizeof hashes[0],
      "Arrays should have same length");
  uint32_t offset = 0;
  for (uint32_t idx = 0; idx < strCount; idx++) {
    predefinedStrings_[idx] =
        identifierTable_
            .registerLazyIdentifier(
                {&buffer[offset], strLengths[idx]}, hashes[idx])
            .unsafeGetRaw();
    offset += strLengths[idx];
  }

  constexpr uint32_t symCount = sizeof symLengths / sizeof symLengths[0];
  for (uint32_t idx = 0; idx < symCount; ++idx) {
    // Register at an offset of strCount to account for existing hashed strings.
    predefinedStrings_[strCount + idx] =
        identifierTable_
            .createExternalLazySymbol(
                ASCIIRef{&buffer[offset], symLengths[idx]})
            .unsafeGetRaw();
    offset += symLengths[idx];
  }

  assert(
      !getTopGCScope() &&
      "There shouldn't be any handles allocated during initializing the predefined strings");
}

void Runtime::initCharacterStrings() {
  GCScope gc(this);
  auto marker = gc.createMarker();
  charStrings_.reserve(256);
  for (char16_t ch = 0; ch < 256; ++ch) {
    gc.flushToMarker(marker);
    charStrings_.push_back(allocateCharacterString(ch).getHermesValue());
  }
}

Handle<StringPrimitive> Runtime::allocateCharacterString(char16_t ch) {
  // This can in theory throw when called out of initialization phase.
  // However there is only that many character strings and in practice this
  // is not a problem.  Note that we allocate these as "long-lived" objects,
  // so we don't have to scan the charStrings_ array in
  // young-generation collections.

  PinnedHermesValue strRes;
  if (LLVM_LIKELY(ch < 128)) {
    strRes = ignoreAllocationFailure(
        StringPrimitive::createLongLived(this, ASCIIRef(ch)));
  } else {
    strRes = ignoreAllocationFailure(
        StringPrimitive::createLongLived(this, UTF16Ref(ch)));
  }
  return makeHandle<StringPrimitive>(strRes);
}

Handle<StringPrimitive> Runtime::getCharacterString(char16_t ch) {
  if (LLVM_LIKELY(ch < 256))
    return Handle<StringPrimitive>::vmcast(&charStrings_[ch]);

  return makeHandle<StringPrimitive>(
      ignoreAllocationFailure(StringPrimitive::create(this, UTF16Ref(ch))));
}

// Store all object and symbol ids in a static table to conserve code size.
static const struct {
  uint16_t object, method;
#ifndef NDEBUG
  const char *name;
#define BUILTIN_METHOD(object, method) \
  {(uint16_t)Predefined::object,       \
   (uint16_t)Predefined::method,       \
   #object "::" #method},
#else
#define BUILTIN_METHOD(object, method) \
  {(uint16_t)Predefined::object, (uint16_t)Predefined::method},
#endif
} builtinMethods[] = {
#include "hermes/Inst/Builtins.def"
};

static_assert(
    sizeof(builtinMethods) / sizeof(builtinMethods[0]) ==
        inst::BuiltinMethod::_count,
    "builtin method table mismatch");

ExecutionStatus Runtime::forEachBuiltin(const std::function<ExecutionStatus(
                                            unsigned methodIndex,
                                            Predefined objectName,
                                            Handle<JSObject> &object,
                                            SymbolID methodID)> &callback) {
  MutableHandle<JSObject> lastObject{this};
  Predefined lastObjectName = Predefined::_PREDEFINED_COUNT;

  for (unsigned methodIndex = 0; methodIndex < inst::BuiltinMethod::_count;
       ++methodIndex) {
    GCScopeMarkerRAII marker{this};
    DEBUG(llvm::dbgs() << builtinMethods[methodIndex].name << "\n");
    // Find the object first, if it changed.
    auto objectName = (Predefined)builtinMethods[methodIndex].object;
    if (objectName != lastObjectName) {
      auto objectID = getPredefinedSymbolID(objectName);
      auto cr = JSObject::getNamed(getGlobal(), this, objectID);
      assert(
          cr.getStatus() != ExecutionStatus::EXCEPTION &&
          "getNamed() of builtin object failed");
      assert(
          vmisa<JSObject>(cr.getValue()) &&
          "getNamed() of builtin object must be an object");

      lastObject = vmcast<JSObject>(cr.getValue());
      lastObjectName = objectName;
    }

    // Find the method.
    auto methodID =
        getPredefinedSymbolID((Predefined)builtinMethods[methodIndex].method);

    ExecutionStatus status =
        callback(methodIndex, objectName, lastObject, methodID);
    if (status != ExecutionStatus::RETURNED) {
      return ExecutionStatus::EXCEPTION;
    }
  }
  return ExecutionStatus::RETURNED;
}

void Runtime::initBuiltinTable() {
  GCScopeMarkerRAII gcScope{this};

  builtins_.resize(inst::BuiltinMethod::_count);

  (void)forEachBuiltin([this](
                           unsigned methodIndex,
                           Predefined /* objectName */,
                           Handle<JSObject> &currentObject,
                           SymbolID methodID) {
    auto cr = JSObject::getNamed(currentObject, this, methodID);
    assert(
        cr.getStatus() != ExecutionStatus::EXCEPTION &&
        "getNamed() of builtin method failed");
    assert(
        vmisa<NativeFunction>(cr.getValue()) &&
        "getNamed() of builtin method must be a NativeFunction");
    builtins_[methodIndex] = vmcast<NativeFunction>(cr.getValue());
    return ExecutionStatus::RETURNED;
  });
}

ExecutionStatus Runtime::assertBuiltinsUnmodified() {
  assert(!builtinsFrozen_ && "Builtins are already frozen.");
  GCScope gcScope(this);

  return forEachBuiltin([this](
                            unsigned methodIndex,
                            Predefined /* objectName */,
                            Handle<JSObject> &currentObject,
                            SymbolID methodID) {
    auto cr = JSObject::getNamed(currentObject, this, methodID);
    assert(
        cr.getStatus() != ExecutionStatus::EXCEPTION &&
        "getNamed() of builtin method failed");
    // Check if the builtin is overridden.
    auto currentBuiltin = dyn_vmcast<NativeFunction>(cr.getValue());
    if (!currentBuiltin || currentBuiltin != builtins_[methodIndex]) {
      return raiseTypeError(
          "Cannot execute a bytecode compiled with -fstatic-builtins when builtin functions are overriden.");
    }
    return ExecutionStatus::RETURNED;
  });
}

void Runtime::freezeBuiltins() {
  assert(!builtinsFrozen_ && "Builtins are already frozen.");
  GCScope gcScope{this};

  // A list storing all the object ids that we will freeze on the global object
  // in the end.
  std::vector<SymbolID> objectList;
  // A list storing all the method ids on the same object that we will freeze on
  // each object.
  std::vector<SymbolID> methodList;

  (void)forEachBuiltin([this, &objectList, &methodList](
                           unsigned methodIndex,
                           Predefined objectName,
                           Handle<JSObject> &currentObject,
                           SymbolID methodID) {
    methodList.push_back(methodID);
    // This is the last method on current object.
    if (methodIndex + 1 == inst::BuiltinMethod::_count ||
        objectName != (Predefined)builtinMethods[methodIndex + 1].object) {
      // Store the object id in the object set.
      SymbolID objectID = getPredefinedSymbolID(objectName);
      objectList.push_back(objectID);
      // Freeze all methods on the current object.
      JSObject::makePropertiesReadOnlyWithoutTransitions(
          currentObject, this, llvm::ArrayRef<SymbolID>(methodList));
      methodList.clear();
    }
    return ExecutionStatus::RETURNED;
  });

  // Freeze all builtin objects on the global object.
  JSObject::makePropertiesReadOnlyWithoutTransitions(
      getGlobal(), this, llvm::ArrayRef<SymbolID>(objectList));

  builtinsFrozen_ = true;
}

uint64_t Runtime::gcStableHashHermesValue(Handle<HermesValue> value) {
  switch (value->getTag()) {
    case ObjectTag: {
      // For objects, because pointers can move, we need a unique ID
      // that does not change for each object.
      auto id = JSObject::getObjectID(vmcast<JSObject>(*value), this);
      return llvm::hash_value(id);
    }
    case StrTag: {
      // For strings, we hash the string content.
      auto strView = StringPrimitive::createStringView(
          this, Handle<StringPrimitive>::vmcast(value));
      return llvm::hash_combine_range(strView.begin(), strView.end());
    }
    default:
      assert(!value->isPointer() && "Unhandled pointer type");
      if (value->isNumber() && value->getNumber() == 0) {
        // To normalize -0 to 0.
        return 0;
      } else {
        // For everything else, we just take advantage of HermesValue.
        return llvm::hash_value(value->getRaw());
      }
  }
}

bool Runtime::symbolEqualsToStringPrim(SymbolID id, StringPrimitive *strPrim) {
  auto view = identifierTable_.getStringView(this, id);
  return strPrim->equals(view);
}

void Runtime::dumpCallFrames(llvm::raw_ostream &OS) {
  OS << "== Call Frames ==\n";
  const PinnedHermesValue *next = getStackPointer();
  unsigned i = 0;
  for (StackFramePtr sf : getStackFrames()) {
    OS << i++ << " ";
    if (auto *closure = sf.getCalleeClosure()) {
      OS << cellKindStr(closure->getKind()) << " ";
    }
    if (auto *cb = sf.getCalleeCodeBlock()) {
      OS << formatSymbolID(cb->getName()) << " ";
    }
    dumpStackFrame(sf, OS, next);
    next = sf.ptr();
  }
}

LLVM_ATTRIBUTE_NOINLINE
void Runtime::dumpCallFrames() {
  dumpCallFrames(llvm::errs());
}

StackRuntime::StackRuntime(
    StorageProvider *provider,
    const RuntimeConfig &config)
    : Runtime(provider, config) {}

/// Serialize a SymbolID.
llvm::raw_ostream &operator<<(
    llvm::raw_ostream &OS,
    Runtime::FormatSymbolID format) {
  if (!format.symbolID.isValid())
    return OS << "SymbolID(INVALID)";

  OS << "SymbolID("
     << (format.symbolID.isExternal() ? "(External)" : "(Internal)")
     << format.symbolID.unsafeGetIndex() << " \"";

  llvm::SmallString<16> buf;
  format.runtime->getIdentifierTable().debugGetSymbolName(
      format.runtime, format.symbolID, buf);

  OS << buf;
  return OS << "\")";
}

} // namespace vm
} // namespace hermes
