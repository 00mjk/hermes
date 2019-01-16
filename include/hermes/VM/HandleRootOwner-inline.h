#ifndef HERMES_VM_HANDLEROOTOWNER_INLINE_H
#define HERMES_VM_HANDLEROOTOWNER_INLINE_H

#include "hermes/VM/Handle.h"
#include "hermes/VM/HandleRootOwner.h"

namespace hermes {
namespace vm {

inline Handle<HermesValue> HandleRootOwner::makeHandle(HermesValue value) {
  return Handle<HermesValue>(this, value);
}
template <class T>
inline Handle<T> HandleRootOwner::makeHandle(T *p) {
  return Handle<T>(this, p);
}
template <class T>
inline Handle<T> HandleRootOwner::makeHandle(HermesValue value) {
  return Handle<T>(this, vmcast<T>(value));
}

template <class T>
inline Handle<T> HandleRootOwner::makeHandle(const GCPointer<T> &p) {
  return Handle<T>(this, p);
}
inline Handle<SymbolID> HandleRootOwner::makeHandle(SymbolID value) {
  return Handle<SymbolID>(this, value);
}
inline MutableHandle<HermesValue> HandleRootOwner::makeMutableHandle(
    HermesValue value) {
  return MutableHandle<HermesValue>(this, value);
}
template <class T>
inline MutableHandle<T> HandleRootOwner::makeMutableHandle(T *p) {
  return MutableHandle<T>(this, p);
}
template <class T>
inline MutableHandle<T> HandleRootOwner::makeMutableHandle(
    const GCPointer<T> &p) {
  return MutableHandle<T>(this, p);
}

template <class T>
inline Handle<T> HandleRootOwner::makeNullHandle() {
  return Handle<T>::vmcast_or_null(&nullPointer_);
}

inline Handle<HermesValue> HandleRootOwner::getUndefinedValue() {
  return Handle<HermesValue>(&undefinedValue_);
}

inline Handle<HermesValue> HandleRootOwner::getNullValue() {
  return Handle<HermesValue>(&nullValue_);
}

inline Handle<HermesValue> HandleRootOwner::getBoolValue(bool b) {
  return Handle<HermesValue>(b ? &trueValue_ : &falseValue_);
}

inline PinnedHermesValue *HandleRootOwner::newHandle(HermesValue value) {
  assert(topGCScope_ && "no active GCScope");
  return topGCScope_->newHandle(value);
}

} // namespace vm
} // namespace hermes

#endif
