/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the LICENSE
 * file in the root directory of this source tree.
 */
//===----------------------------------------------------------------------===//
/// \file
/// ES6.0 23.2 Initialize the Set constructor.
//===----------------------------------------------------------------------===//

#include "JSLibInternal.h"

#include "hermes/VM/StringPrimitive.h"

namespace hermes {
namespace vm {

/// @name Set
/// @{

/// ES6.0 23.2.1  Set() invoked as a function and as a constructor.
static CallResult<HermesValue>
setConstructor(void *, Runtime *runtime, NativeArgs args);

/// @}

/// @name Set.prototype
/// @{

/// ES6.0 23.2.3.1.
static CallResult<HermesValue>
setPrototypeAdd(void *, Runtime *runtime, NativeArgs args);

/// ES6.0 23.2.3.2.
static CallResult<HermesValue>
setPrototypeClear(void *, Runtime *runtime, NativeArgs args);

// TODO: Implement ES6.0 23.2.3.3: get Set [ @@species ]

/// ES6.0 23.2.3.4.
static CallResult<HermesValue>
setPrototypeDelete(void *, Runtime *runtime, NativeArgs args);

/// ES6.0 23.2.3.5.
static CallResult<HermesValue>
setPrototypeEntries(void *, Runtime *runtime, NativeArgs args);

/// ES6.0 23.2.3.6.
static CallResult<HermesValue>
setPrototypeForEach(void *, Runtime *runtime, NativeArgs args);

/// ES6.0 23.2.3.7.
static CallResult<HermesValue>
setPrototypeHas(void *, Runtime *runtime, NativeArgs args);

/// ES6.0 23.2.3.9.
static CallResult<HermesValue>
setPrototypeSizeGetter(void *, Runtime *runtime, NativeArgs args);

/// ES6.0 23.2.3.10.
static CallResult<HermesValue>
setPrototypeValues(void *, Runtime *runtime, NativeArgs args);

// TODO: Implement ES6.0 23.2.3.11: Set.prototype [ @@iterator ]()

// TODO: Implement ES6.0 23.2.3.12: Set.prototype [ @@toStringTag ]

/// @}

/// @name SetIterator.prototype
/// @{

/// ES6.0 23.2.5.2.1.
static CallResult<HermesValue>
setIteratorPrototypeNext(void *, Runtime *runtime, NativeArgs args);

/// @}

Handle<JSObject> createSetConstructor(Runtime *runtime) {
  auto setPrototype = Handle<JSSet>::vmcast(&runtime->setPrototype);

  // Set.prototype.xxx methods.
  defineMethod(
      runtime,
      setPrototype,
      runtime->getPredefinedSymbolID(Predefined::add),
      nullptr,
      setPrototypeAdd,
      1);

  defineMethod(
      runtime,
      setPrototype,
      runtime->getPredefinedSymbolID(Predefined::clear),
      nullptr,
      setPrototypeClear,
      0);

  defineMethod(
      runtime,
      setPrototype,
      runtime->getPredefinedSymbolID(Predefined::deleteStr),
      nullptr,
      setPrototypeDelete,
      1);

  defineMethod(
      runtime,
      setPrototype,
      runtime->getPredefinedSymbolID(Predefined::entries),
      nullptr,
      setPrototypeEntries,
      0);

  defineMethod(
      runtime,
      setPrototype,
      runtime->getPredefinedSymbolID(Predefined::forEach),
      nullptr,
      setPrototypeForEach,
      1);

  defineMethod(
      runtime,
      setPrototype,
      runtime->getPredefinedSymbolID(Predefined::has),
      nullptr,
      setPrototypeHas,
      1);

  defineAccessor(
      runtime,
      setPrototype,
      runtime->getPredefinedSymbolID(Predefined::size),
      nullptr,
      setPrototypeSizeGetter,
      nullptr,
      false,
      true);

  defineMethod(
      runtime,
      setPrototype,
      runtime->getPredefinedSymbolID(Predefined::values),
      nullptr,
      setPrototypeValues,
      0);

  DefinePropertyFlags dpf{};
  dpf.setEnumerable = 1;
  dpf.setWritable = 1;
  dpf.setConfigurable = 1;
  dpf.setValue = 1;
  dpf.enumerable = 0;
  dpf.writable = 1;
  dpf.configurable = 1;

  // Use the same valuesMethod for both keys() and values().
  auto propValue = runtime->makeHandle<NativeFunction>(
      runtime->ignoreAllocationFailure(JSObject::getNamed(
          setPrototype,
          runtime,
          runtime->getPredefinedSymbolID(Predefined::values))));
  runtime->ignoreAllocationFailure(JSObject::defineOwnProperty(
      setPrototype,
      runtime,
      runtime->getPredefinedSymbolID(Predefined::keys),
      dpf,
      propValue));
  runtime->ignoreAllocationFailure(JSObject::defineOwnProperty(
      setPrototype,
      runtime,
      runtime->getPredefinedSymbolID(Predefined::SymbolIterator),
      dpf,
      propValue));

  dpf = DefinePropertyFlags::getDefaultNewPropertyFlags();
  dpf.writable = 0;
  dpf.enumerable = 0;
  defineProperty(
      runtime,
      setPrototype,
      runtime->getPredefinedSymbolID(Predefined::SymbolToStringTag),
      runtime->getPredefinedStringHandle(Predefined::Set),
      dpf);

  auto cons = defineSystemConstructor<JSSet>(
      runtime,
      runtime->getPredefinedSymbolID(Predefined::Set),
      setConstructor,
      setPrototype,
      0,
      CellKind::SetKind);

  return cons;
}

static CallResult<HermesValue>
setConstructor(void *, Runtime *runtime, NativeArgs args) {
  GCScope gcScope{runtime};
  if (LLVM_UNLIKELY(!args.isConstructorCall())) {
    return runtime->raiseTypeError("Constructor Set requires 'new'");
  }
  auto selfHandle = args.dyncastThis<JSSet>(runtime);
  if (LLVM_UNLIKELY(!selfHandle)) {
    return runtime->raiseTypeError(
        "Set Constructor only applies to Set object");
  }

  JSSet::initializeStorage(selfHandle, runtime);

  if (args.getArgCount() == 0 || args.getArg(0).isUndefined() ||
      args.getArg(0).isNull()) {
    return selfHandle.getHermesValue();
  }

  auto propRes = JSObject::getNamed(
      selfHandle, runtime, runtime->getPredefinedSymbolID(Predefined::add));
  if (LLVM_UNLIKELY(propRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }

  // ES6.0 23.2.1.1.7: Cache adder across all iterations of the loop.
  auto adder =
      Handle<Callable>::dyn_vmcast(runtime, runtime->makeHandle(*propRes));
  if (!adder) {
    return runtime->raiseTypeError("Property 'add' for Set is not callable");
  }

  auto iterRes = getIterator(runtime, args.getArgHandle(runtime, 0));
  if (LLVM_UNLIKELY(iterRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  auto iter = toHandle(runtime, std::move(*iterRes));

  // Iterate the array and add every element.
  MutableHandle<JSObject> tmpHandle{runtime};
  auto marker = gcScope.createMarker();

  // Check the length of the array after every iteration,
  // to allow for the fact that the length could be modified during iteration.
  for (;;) {
    gcScope.flushToMarker(marker);
    CallResult<Handle<JSObject>> nextRes = iteratorStep(runtime, iter);
    if (LLVM_UNLIKELY(nextRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    if (!*nextRes) {
      // Done with iteration.
      return selfHandle.getHermesValue();
    }
    tmpHandle = vmcast<JSObject>(nextRes->getHermesValue());
    auto nextValueRes = JSObject::getNamed(
        tmpHandle, runtime, runtime->getPredefinedSymbolID(Predefined::value));
    if (LLVM_UNLIKELY(nextValueRes == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }

    if (LLVM_UNLIKELY(
            Callable::executeCall1(adder, runtime, selfHandle, *nextValueRes) ==
            ExecutionStatus::EXCEPTION)) {
      return iteratorCloseAndRethrow(runtime, iter);
    }
  }

  return selfHandle.getHermesValue();
}

static CallResult<HermesValue>
setPrototypeAdd(void *, Runtime *runtime, NativeArgs args) {
  auto selfHandle = args.dyncastThis<JSSet>(runtime);
  if (LLVM_UNLIKELY(!selfHandle)) {
    return runtime->raiseTypeError(
        "Non-Set object called on Set.prototype.add");
  }
  if (LLVM_UNLIKELY(!selfHandle->isInitialized())) {
    return runtime->raiseTypeError(
        "Method Set.prototype.add called on incompatible receiver");
  }
  auto valueHandle = args.getArgHandle(runtime, 0);
  JSSet::addValue(selfHandle, runtime, valueHandle, valueHandle);
  return selfHandle.getHermesValue();
}

static CallResult<HermesValue>
setPrototypeClear(void *, Runtime *runtime, NativeArgs args) {
  auto selfHandle = args.dyncastThis<JSSet>(runtime);
  if (LLVM_UNLIKELY(!selfHandle)) {
    return runtime->raiseTypeError(
        "Non-Set object called on Set.prototype.clear");
  }
  if (LLVM_UNLIKELY(!selfHandle->isInitialized())) {
    return runtime->raiseTypeError(
        "Method Set.prototype.clear called on incompatible receiver");
  }
  JSSet::clear(selfHandle, runtime);
  return HermesValue::encodeUndefinedValue();
}

static CallResult<HermesValue>
setPrototypeDelete(void *, Runtime *runtime, NativeArgs args) {
  auto selfHandle = args.dyncastThis<JSSet>(runtime);
  if (LLVM_UNLIKELY(!selfHandle)) {
    return runtime->raiseTypeError(
        "Non-Set object called on Set.prototype.delete");
  }
  if (LLVM_UNLIKELY(!selfHandle->isInitialized())) {
    return runtime->raiseTypeError(
        "Method Set.prototype.delete called on incompatible receiver");
  }
  return HermesValue::encodeBoolValue(
      JSSet::deleteKey(selfHandle, runtime, args.getArgHandle(runtime, 0)));
}

static CallResult<HermesValue>
setPrototypeEntries(void *, Runtime *runtime, NativeArgs args) {
  auto selfHandle = args.dyncastThis<JSSet>(runtime);
  if (LLVM_UNLIKELY(!selfHandle)) {
    return runtime->raiseTypeError(
        "Non-Set object called on Set.prototype.entries");
  }
  if (LLVM_UNLIKELY(!selfHandle->isInitialized())) {
    return runtime->raiseTypeError(
        "Method Set.prototype.entries called on incompatible receiver");
  }
  auto mapRes = JSSetIterator::create(
      runtime, Handle<JSObject>::vmcast(&runtime->setIteratorPrototype));
  if (LLVM_UNLIKELY(mapRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  auto iterator = runtime->makeHandle<JSSetIterator>(*mapRes);
  iterator->initializeIterator(
      &runtime->getHeap(), selfHandle, IterationKind::Entry);
  return iterator.getHermesValue();
}

static CallResult<HermesValue>
setPrototypeForEach(void *, Runtime *runtime, NativeArgs args) {
  auto selfHandle = args.dyncastThis<JSSet>(runtime);
  if (LLVM_UNLIKELY(!selfHandle)) {
    return runtime->raiseTypeError(
        "Non-Set object called on Set.prototype.forEach");
  }
  if (LLVM_UNLIKELY(!selfHandle->isInitialized())) {
    return runtime->raiseTypeError(
        "Method Set.prototype.forEach called on incompatible receiver");
  }
  auto callbackfn = args.dyncastArg<Callable>(runtime, 0);
  if (LLVM_UNLIKELY(!callbackfn)) {
    return runtime->raiseTypeError(
        "callbackfn must be Callable inSet.prototype.forEach");
  }
  auto thisArg = args.getArgHandle(runtime, 1);
  if (LLVM_UNLIKELY(
          JSSet::forEach(selfHandle, runtime, callbackfn, thisArg) ==
          ExecutionStatus::EXCEPTION))
    return ExecutionStatus::EXCEPTION;
  return HermesValue::encodeUndefinedValue();
}

static CallResult<HermesValue>
setPrototypeHas(void *, Runtime *runtime, NativeArgs args) {
  auto selfHandle = args.dyncastThis<JSSet>(runtime);
  if (LLVM_UNLIKELY(!selfHandle)) {
    return runtime->raiseTypeError(
        "Non-Set object called on Set.prototype.has");
  }
  if (LLVM_UNLIKELY(!selfHandle->isInitialized())) {
    return runtime->raiseTypeError(
        "Method Set.prototype.has called on incompatible receiver");
  }
  return HermesValue::encodeBoolValue(
      JSSet::hasKey(selfHandle, runtime, args.getArgHandle(runtime, 0)));
}

static CallResult<HermesValue>
setPrototypeSizeGetter(void *, Runtime *runtime, NativeArgs args) {
  auto self = dyn_vmcast<JSSet>(args.getThisArg());
  if (LLVM_UNLIKELY(!self)) {
    return runtime->raiseTypeError(
        "Non-Set object called on Set.prototype.size");
  }
  if (LLVM_UNLIKELY(!self->isInitialized())) {
    return runtime->raiseTypeError(
        "Method Set.prototype.size called on incompatible receiver");
  }
  return HermesValue::encodeNumberValue(JSSet::getSize(self));
}

static CallResult<HermesValue>
setPrototypeValues(void *, Runtime *runtime, NativeArgs args) {
  auto selfHandle = args.dyncastThis<JSSet>(runtime);
  if (LLVM_UNLIKELY(!selfHandle)) {
    return runtime->raiseTypeError(
        "Non-Set object called on Set.prototype.values");
  }
  if (LLVM_UNLIKELY(!selfHandle->isInitialized())) {
    return runtime->raiseTypeError(
        "Method Set.prototype.values called on incompatible receiver");
  }
  auto mapRes = JSSetIterator::create(
      runtime, Handle<JSObject>::vmcast(&runtime->setIteratorPrototype));
  if (LLVM_UNLIKELY(mapRes == ExecutionStatus::EXCEPTION)) {
    return ExecutionStatus::EXCEPTION;
  }
  auto iterator = runtime->makeHandle<JSSetIterator>(*mapRes);
  iterator->initializeIterator(
      &runtime->getHeap(), selfHandle, IterationKind::Value);
  return iterator.getHermesValue();
}

Handle<JSObject> createSetIteratorPrototype(Runtime *runtime) {
  auto protoHandle = toHandle(
      runtime,
      JSObject::create(
          runtime, Handle<JSObject>::vmcast(&runtime->iteratorPrototype)));
  defineMethod(
      runtime,
      protoHandle,
      runtime->getPredefinedSymbolID(Predefined::next),
      nullptr,
      setIteratorPrototypeNext,
      0);

  auto dpf = DefinePropertyFlags::getDefaultNewPropertyFlags();
  dpf.writable = 0;
  dpf.enumerable = 0;
  defineProperty(
      runtime,
      protoHandle,
      runtime->getPredefinedSymbolID(Predefined::SymbolToStringTag),
      runtime->getPredefinedStringHandle(Predefined::SetIterator),
      dpf);

  return protoHandle;
}

static CallResult<HermesValue>
setIteratorPrototypeNext(void *, Runtime *runtime, NativeArgs args) {
  auto selfHandle = args.dyncastThis<JSSetIterator>(runtime);
  if (LLVM_UNLIKELY(!selfHandle)) {
    return runtime->raiseTypeError(
        "Non-SetIterator object called on SetIterator.prototype.next");
  }
  if (LLVM_UNLIKELY(!selfHandle->isInitialized())) {
    return runtime->raiseTypeError(
        "Method SetIterator.prototype.next called on incompatible receiver");
  }
  auto cr = JSSetIterator::nextElement(selfHandle, runtime);
  if (cr == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  return *cr;
}
} // namespace vm
} // namespace hermes
