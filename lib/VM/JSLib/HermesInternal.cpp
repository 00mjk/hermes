/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the LICENSE
 * file in the root directory of this source tree.
 */
#include "JSLibInternal.h"

#include "hermes/BCGen/HBC/BytecodeFileFormat.h"
#include "hermes/VM/JSArrayBuffer.h"
#include "hermes/VM/JSTypedArray.h"
#include "hermes/VM/JSWeakMapImpl.h"

namespace hermes {
namespace vm {

namespace {

/// \return a SymbolID  for a given C string \p s.
static inline CallResult<Handle<SymbolID>> symbolForCStr(
    Runtime *rt,
    const char *s) {
  return rt->getIdentifierTable().getSymbolHandle(rt, ASCIIRef{s, strlen(s)});
}

// ES7 24.1.1.3
CallResult<HermesValue>
hermesInternalDetachArrayBuffer(void *, Runtime *runtime, NativeArgs args) {
  auto buffer = args.dyncastArg<JSArrayBuffer>(runtime, 0);
  if (!buffer) {
    return runtime->raiseTypeError(
        "Cannot use detachArrayBuffer on something which "
        "is not an ArrayBuffer foo");
  }
  buffer->detach(&runtime->getHeap());
  // "void" return
  return HermesValue::encodeUndefinedValue();
}

/// An API for JS code to request a heap snapshot at any point of execution.
/// This runs a garbage collection first, in order to not show any garbage in
/// the snapshot.
CallResult<HermesValue>
hermesInternalCreateHeapSnapshot(void *, Runtime *runtime, NativeArgs args) {
  bool compact = true;
  if (args.getArgCount() >= 1) {
    compact = toBoolean(args.getArg(0));
  }
  runtime->getHeap().createSnapshot(llvm::errs(), compact);
  return HermesValue::encodeUndefinedValue();
}

CallResult<HermesValue>
hermesInternalGetEpilogues(void *, Runtime *runtime, NativeArgs args) {
  // Create outer array with one element per module.
  auto eps = runtime->getEpilogues();
  auto outerLen = eps.size();
  auto outerResult = JSArray::create(runtime, outerLen, outerLen);

  if (outerResult == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  auto outer = toHandle(runtime, std::move(*outerResult));
  if (outer->setStorageEndIndex(outer, runtime, outerLen) ==
      ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  // Set each element to a Uint8Array holding the epilogue for that module.
  for (unsigned i = 0; i < outerLen; ++i) {
    auto innerLen = eps[i].size();
    if (innerLen != 0) {
      auto result = JSTypedArray<uint8_t, CellKind::Uint8ArrayKind>::allocate(
          runtime, innerLen);
      if (result == ExecutionStatus::EXCEPTION) {
        return ExecutionStatus::EXCEPTION;
      }
      auto ta = result.getValue();
      std::memcpy(ta->begin(), eps[i].begin(), innerLen);
      JSArray::unsafeSetExistingElementAt(
          *outer, runtime, i, ta.getHermesValue());
    }
  }
  return HermesValue::encodeObjectValue(*outer);
}

/// Set the parent of an object failing silently on any error.
CallResult<HermesValue>
silentObjectSetPrototypeOf(void *, Runtime *runtime, NativeArgs args) {
  JSObject *O = dyn_vmcast<JSObject>(args.getArg(0));
  if (!O)
    return HermesValue::encodeUndefinedValue();

  JSObject *parent;
  HermesValue V = args.getArg(1);
  if (V.isNull())
    parent = nullptr;
  else if (V.isObject())
    parent = vmcast<JSObject>(V);
  else
    return HermesValue::encodeUndefinedValue();

  JSObject::setParent(O, runtime, parent);

  // Ignore exceptions.
  runtime->clearThrownValue();

  return HermesValue::encodeUndefinedValue();
}

/// Used for testing, determines how many live values
/// are in the given WeakMap or WeakSet.
CallResult<HermesValue>
hermesInternalGetWeakSize(void *, Runtime *runtime, NativeArgs args) {
  auto M = args.dyncastArg<JSWeakMap>(runtime, 0);
  if (M) {
    return HermesValue::encodeNumberValue(JSWeakMap::debugGetSize(*M));
  }

  auto S = args.dyncastArg<JSWeakSet>(runtime, 0);
  if (S) {
    return HermesValue::encodeNumberValue(JSWeakSet::debugGetSize(*S));
  }

  return runtime->raiseTypeError(
      "getWeakSize can only be called on a WeakMap/WeakSet");
}

/// \return an object containing various instrumented statistics.
CallResult<HermesValue>
hermesInternalGetInstrumentedStats(void *, Runtime *runtime, NativeArgs args) {
  GCScope gcScope(runtime);
  auto resultHandle = toHandle(runtime, JSObject::create(runtime));
  MutableHandle<> tmpHandle{runtime};

  namespace P = Predefined;
/// Adds a property to \c resultHandle. \p KEY provides its name as a \c
/// Predefined enum value, and its value is rooted in \p VALUE.  If property
/// definition fails, the exceptional execution status will be propogated to the
/// outer function.
#define SET_PROP(KEY, VALUE)                                   \
  do {                                                         \
    GCScopeMarkerRAII marker{gcScope};                         \
    tmpHandle = HermesValue::encodeDoubleValue(VALUE);         \
    auto status = JSObject::defineNewOwnProperty(              \
        resultHandle,                                          \
        runtime,                                               \
        Predefined::getSymbolID(KEY),                          \
        PropertyFlags::defaultNewNamedPropertyFlags(),         \
        tmpHandle);                                            \
    if (LLVM_UNLIKELY(status == ExecutionStatus::EXCEPTION)) { \
      return ExecutionStatus::EXCEPTION;                       \
    }                                                          \
  } while (false)

  auto &stats = runtime->getRuntimeStats();

  // Ensure that the timers measuring the current execution are up to date.
  stats.flushPendingTimers();

  SET_PROP(P::js_hostFunctionTime, stats.hostFunction.wallDuration);
  SET_PROP(P::js_hostFunctionCPUTime, stats.hostFunction.cpuDuration);
  SET_PROP(P::js_hostFunctionCount, stats.hostFunction.count);

  SET_PROP(P::js_evaluateJSTime, stats.evaluateJS.wallDuration);
  SET_PROP(P::js_evaluateJSCPUTime, stats.evaluateJS.cpuDuration);
  SET_PROP(P::js_evaluateJSCount, stats.evaluateJS.count);

  SET_PROP(P::js_incomingFunctionTime, stats.incomingFunction.wallDuration);
  SET_PROP(P::js_incomingFunctionCPUTime, stats.incomingFunction.cpuDuration);
  SET_PROP(P::js_incomingFunctionCount, stats.incomingFunction.count);

  auto makeHermesTime = [](double host, double eval, double incoming) {
    return eval - host + incoming;
  };

  SET_PROP(
      P::js_hermesTime,
      makeHermesTime(
          stats.hostFunction.wallDuration,
          stats.evaluateJS.wallDuration,
          stats.incomingFunction.wallDuration));
  SET_PROP(
      P::js_hermesCPUTime,
      makeHermesTime(
          stats.hostFunction.cpuDuration,
          stats.evaluateJS.cpuDuration,
          stats.incomingFunction.cpuDuration));

  if (stats.shouldSample) {
    SET_PROP(
        P::js_hermesThreadMinorFaults,
        makeHermesTime(
            stats.hostFunction.sampled.threadMinorFaults,
            stats.evaluateJS.sampled.threadMinorFaults,
            stats.incomingFunction.sampled.threadMinorFaults));
    SET_PROP(
        P::js_hermesThreadMajorFaults,
        makeHermesTime(
            stats.hostFunction.sampled.threadMajorFaults,
            stats.evaluateJS.sampled.threadMajorFaults,
            stats.incomingFunction.sampled.threadMajorFaults));
  }

  const auto &heap = runtime->getHeap();
  SET_PROP(P::js_numGCs, heap.getNumGCs());
  SET_PROP(P::js_gcCPUTime, heap.getGCCPUTime());
  SET_PROP(P::js_gcTime, heap.getGCTime());

#undef SET_PROP

/// Adds a property to \c resultHandle. \p KEY provides its name as a C string,
/// and its value is rooted in \p VALUE.  If property definition fails, the
/// exceptional execution status will be propogated to the outer function.
#define SET_PROP_NEW(KEY, VALUE)                               \
  do {                                                         \
    auto keySym = symbolForCStr(runtime, KEY);                 \
    if (LLVM_UNLIKELY(keySym == ExecutionStatus::EXCEPTION)) { \
      return ExecutionStatus::EXCEPTION;                       \
    }                                                          \
    tmpHandle = HermesValue::encodeDoubleValue(VALUE);         \
    auto status = JSObject::defineNewOwnProperty(              \
        resultHandle,                                          \
        runtime,                                               \
        **keySym,                                              \
        PropertyFlags::defaultNewNamedPropertyFlags(),         \
        tmpHandle);                                            \
    if (LLVM_UNLIKELY(status == ExecutionStatus::EXCEPTION)) { \
      return ExecutionStatus::EXCEPTION;                       \
    }                                                          \
  } while (false)

  if (runtime->getRuntimeStats().shouldSample) {
    size_t bytecodePagesResident = 0;
    size_t bytecodePagesResidentRuns = 0;
    for (auto &module : runtime->getRuntimeModules()) {
      auto buf = module.getBytecode()->getRawBuffer();
      if (buf.size()) {
        llvm::SmallVector<int, 64> runs;
        int pages = oscompat::pages_in_ram(buf.data(), buf.size(), &runs);
        if (pages >= 0) {
          bytecodePagesResident += pages;
          bytecodePagesResidentRuns += runs.size();
        }
      }
    }
    SET_PROP_NEW("js_bytecodePagesResident", bytecodePagesResident);
    SET_PROP_NEW("js_bytecodePagesResidentRuns", bytecodePagesResidentRuns);
  }

  return resultHandle.getHermesValue();

#undef SET_PROP_NEW
}

/// \return an object mapping keys to runtime property values.
CallResult<HermesValue>
hermesInternalGetRuntimeProperties(void *, Runtime *runtime, NativeArgs args) {
  GCScope gcScope(runtime);
  auto resultHandle = toHandle(runtime, JSObject::create(runtime));
  MutableHandle<> tmpHandle{runtime};

  auto bcVersion = symbolForCStr(runtime, "Bytecode Version");
  if (LLVM_UNLIKELY(bcVersion == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  tmpHandle = HermesValue::encodeDoubleValue(::hermes::hbc::BYTECODE_VERSION);
  auto status = JSObject::defineNewOwnProperty(
      resultHandle,
      runtime,
      **bcVersion,
      PropertyFlags::defaultNewNamedPropertyFlags(),
      tmpHandle);

  if (LLVM_UNLIKELY(status == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  auto builtinsAreFrozen = symbolForCStr(runtime, "Builtins Frozen");
  if (LLVM_UNLIKELY(builtinsAreFrozen == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  tmpHandle = HermesValue::encodeBoolValue(runtime->builtinsAreFrozen());
  status = JSObject::defineNewOwnProperty(
      resultHandle,
      runtime,
      **builtinsAreFrozen,
      PropertyFlags::defaultNewNamedPropertyFlags(),
      tmpHandle);
  if (LLVM_UNLIKELY(status == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  return resultHandle.getHermesValue();
}

/// ES6.0 12.2.9.3 Runtime Semantics: GetTemplateObject ( templateLiteral )
/// Given a template literal, return a template object that looks like this:
/// [cookedString0, cookedString1, ..., raw: [rawString0, rawString1]].
/// This object is frozen, as well as the 'raw' object nested inside.
/// We only pass the parts from the template literal that are needed to
/// construct this object. That is, the raw strings and cooked strings.
/// Arguments: \p templateObjID is the unique id associated with the template
/// object. \p dup is a boolean, when it is true, cooked strings are the same as
/// raw strings. Then raw strings are passed. Finally cooked strings are
/// optionally passed if \p dup is true.
CallResult<HermesValue>
hermesInternalGetTemplateObject(void *, Runtime *runtime, NativeArgs args) {
  if (LLVM_UNLIKELY(args.getArgCount() < 3)) {
    return runtime->raiseTypeError("At least three arguments expected");
  }
  if (LLVM_UNLIKELY(!args.getArg(0).isNumber())) {
    return runtime->raiseTypeError("First argument should be a number");
  }
  if (LLVM_UNLIKELY(!args.getArg(1).isBool())) {
    return runtime->raiseTypeError("Second argument should be a bool");
  }

  GCScope gcScope{runtime};

  // Try finding the template object in the template object cache.
  uint32_t templateObjID = args.getArg(0).getNumberAs<uint32_t>();
  auto savedCB = runtime->getStackFrames().begin()->getSavedCodeBlock();
  if (LLVM_UNLIKELY(!savedCB)) {
    return runtime->raiseTypeError("Cannot be called from native code");
  }
  RuntimeModule *runtimeModule = savedCB->getRuntimeModule();
  JSObject *cachedTemplateObj =
      runtimeModule->findCachedTemplateObject(templateObjID);
  if (cachedTemplateObj) {
    return HermesValue::encodeObjectValue(cachedTemplateObj);
  }

  bool dup = args.getArg(1).getBool();
  if (LLVM_UNLIKELY(!dup && args.getArgCount() % 2 == 1)) {
    return runtime->raiseTypeError(
        "There must be the same number of raw and cooked strings.");
  }
  uint32_t count = dup ? args.getArgCount() - 2 : args.getArgCount() / 2 - 1;

  // Create template object and raw object.
  auto arrRes = JSArray::create(runtime, count, 0);
  if (LLVM_UNLIKELY(arrRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  auto rawObj = runtime->makeHandle<JSObject>(arrRes->getHermesValue());
  auto arrRes2 = JSArray::create(runtime, count, 0);
  if (LLVM_UNLIKELY(arrRes2 == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  auto templateObj = runtime->makeHandle<JSObject>(arrRes2->getHermesValue());

  // Set cooked and raw strings as elements in template object and raw object,
  // respectively.
  DefinePropertyFlags dpf{};
  dpf.setWritable = 1;
  dpf.setConfigurable = 1;
  dpf.setEnumerable = 1;
  dpf.setValue = 1;
  dpf.writable = 0;
  dpf.configurable = 0;
  dpf.enumerable = 1;
  MutableHandle<> idx{runtime};
  MutableHandle<> rawValue{runtime};
  MutableHandle<> cookedValue{runtime};
  uint32_t cookedBegin = dup ? 2 : 2 + count;
  auto marker = gcScope.createMarker();
  for (uint32_t i = 0; i < count; ++i) {
    idx = HermesValue::encodeNumberValue(i);

    cookedValue = args.getArg(cookedBegin + i);
    auto putRes = JSObject::defineOwnComputedPrimitive(
        templateObj, runtime, idx, dpf, cookedValue);
    assert(
        putRes != ExecutionStatus::EXCEPTION && *putRes &&
        "Failed to set cooked value to template object.");

    rawValue = args.getArg(2 + i);
    putRes = JSObject::defineOwnComputedPrimitive(
        rawObj, runtime, idx, dpf, rawValue);
    assert(
        putRes != ExecutionStatus::EXCEPTION && *putRes &&
        "Failed to set raw value to raw object.");

    gcScope.flushToMarker(marker);
  }
  // Make 'length' property on the raw object read-only.
  DefinePropertyFlags readOnlyDPF{};
  readOnlyDPF.setWritable = 1;
  readOnlyDPF.setConfigurable = 1;
  readOnlyDPF.writable = 0;
  readOnlyDPF.configurable = 0;
  auto readOnlyRes = JSObject::defineOwnProperty(
      rawObj,
      runtime,
      Predefined::getSymbolID(Predefined::length),
      readOnlyDPF,
      runtime->getUndefinedValue(),
      PropOpFlags().plusThrowOnError());
  if (LLVM_UNLIKELY(readOnlyRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  if (LLVM_UNLIKELY(!*readOnlyRes)) {
    return runtime->raiseTypeError(
        "Failed to set 'length' property on the raw object read-only.");
  }
  JSObject::preventExtensions(rawObj.get());

  // Set raw object as a read-only non-enumerable property of the template
  // object.
  PropertyFlags constantPF{};
  constantPF.writable = 0;
  constantPF.configurable = 0;
  constantPF.enumerable = 0;
  auto putNewRes = JSObject::defineNewOwnProperty(
      templateObj,
      runtime,
      Predefined::getSymbolID(Predefined::raw),
      constantPF,
      rawObj);
  if (LLVM_UNLIKELY(putNewRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  // Make 'length' property on the template object read-only.
  readOnlyRes = JSObject::defineOwnProperty(
      templateObj,
      runtime,
      Predefined::getSymbolID(Predefined::length),
      readOnlyDPF,
      runtime->getUndefinedValue(),
      PropOpFlags().plusThrowOnError());
  if (LLVM_UNLIKELY(readOnlyRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  if (LLVM_UNLIKELY(!*readOnlyRes)) {
    return runtime->raiseTypeError(
        "Failed to set 'length' property on the raw object read-only.");
  }
  JSObject::preventExtensions(templateObj.get());

  // Cache the template object.
  runtimeModule->cacheTemplateObject(templateObjID, templateObj);

  return templateObj.getHermesValue();
}

/// If the first argument is not an object, throw a type error with the second
/// argument as a message.
///
/// \code
///   HermesInternal.ensureObject = function(value, errorMessage) {...}
/// \endcode
CallResult<HermesValue>
hermesInternalEnsureObject(void *, Runtime *runtime, NativeArgs args) {
  if (LLVM_LIKELY(args.getArg(0).isObject()))
    return HermesValue::encodeUndefinedValue();

  return runtime->raiseTypeError(args.getArgHandle(runtime, 1));
}

CallResult<HermesValue>
hermesInternalTTIReached(void *, Runtime *runtime, NativeArgs args) {
  runtime->ttiReached();
#ifdef HERMESVM_LLVM_PROFILE_DUMP
  __llvm_profile_dump();
  throw jsi::JSINativeException("TTI reached; profiling done");
#endif
#ifdef HERMESVM_PLATFORM_LOGGING
  logGCStats("TTI call");
#endif
  return HermesValue::encodeUndefinedValue();
}

} // namespace

Handle<JSObject> createHermesInternalObject(Runtime *runtime) {
  Handle<JSObject> intern = toHandle(runtime, JSObject::create(runtime));

  DefinePropertyFlags constantDPF{};
  constantDPF.setEnumerable = 1;
  constantDPF.setWritable = 1;
  constantDPF.setConfigurable = 1;
  constantDPF.setValue = 1;
  constantDPF.enumerable = 0;
  constantDPF.writable = 0;
  constantDPF.configurable = 0;

  auto defineInternMethod =
      [&](Predefined::Str symID, NativeFunctionPtr func, uint8_t count = 0) {
        (void)defineMethod(
            runtime,
            intern,
            Predefined::getSymbolID(symID),
            nullptr /* context */,
            func,
            count,
            constantDPF);
      };

  // HermesInternal function properties
  namespace P = Predefined;
  defineInternMethod(P::detachArrayBuffer, hermesInternalDetachArrayBuffer, 1);
  defineInternMethod(P::createHeapSnapshot, hermesInternalCreateHeapSnapshot);
  defineInternMethod(P::getEpilogues, hermesInternalGetEpilogues);
  defineInternMethod(P::silentSetPrototypeOf, silentObjectSetPrototypeOf, 2);
  defineInternMethod(P::getWeakSize, hermesInternalGetWeakSize);
  defineInternMethod(
      P::getInstrumentedStats, hermesInternalGetInstrumentedStats);
  defineInternMethod(
      P::getRuntimeProperties, hermesInternalGetRuntimeProperties);
  defineInternMethod(P::getTemplateObject, hermesInternalGetTemplateObject);
  defineInternMethod(P::ensureObject, hermesInternalEnsureObject, 2);
  defineInternMethod(P::ttiReached, hermesInternalTTIReached);

  // Define the 'require' function.
  runtime->requireFunction = *defineMethod(
      runtime,
      intern,
      Predefined::getSymbolID(Predefined::require),
      nullptr,
      require,
      1,
      constantDPF);

  // Define the 'requireFast' function, which takes a number argument.
  (void)defineMethod(
      runtime,
      intern,
      Predefined::getSymbolID(Predefined::requireFast),
      nullptr,
      requireFast,
      1,
      constantDPF);

  // Make a copy of the original String.prototype.concat implementation that we
  // can use internally.
  // TODO: we can't make HermesInternal.concat a static builtin method now
  // because this method should be called with a meaningful `this`, but
  // CallBuiltin instruction does not support it.
  auto propRes = JSObject::getNamed(
      runtime->makeHandle<JSObject>(runtime->stringPrototype),
      runtime,
      Predefined::getSymbolID(Predefined::concat));
  assert(
      propRes != ExecutionStatus::EXCEPTION && !propRes->isUndefined() &&
      "Failed to get String.prototype.concat.");
  auto putRes = JSObject::defineOwnProperty(
      intern,
      runtime,
      Predefined::getSymbolID(Predefined::concat),
      constantDPF,
      runtime->makeHandle(*propRes));
  assert(
      putRes != ExecutionStatus::EXCEPTION && *putRes &&
      "Failed to set HermesInternal.concat.");
  (void)putRes;

  JSObject::preventExtensions(*intern);

  return intern;
}

} // namespace vm
} // namespace hermes
