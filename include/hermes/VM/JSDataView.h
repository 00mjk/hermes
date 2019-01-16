#ifndef HERMES_VM_JSDATAVIEW_H
#define HERMES_VM_JSDATAVIEW_H

#include "hermes/VM/GCPointer-inline.h"
#include "hermes/VM/GCPointer.h"
#include "hermes/VM/JSArrayBuffer.h"
#include "hermes/VM/JSObject.h"

#include "llvm/Support/Endian.h"
#include "llvm/Support/SwapByteOrder.h"

namespace hermes {
namespace vm {

class JSDataView final : public JSObject {
 public:
  using size_type = JSArrayBuffer::size_type;
  using Super = JSObject;

  static ObjectVTable vt;

  static bool classof(const GCCell *cell) {
    return cell->getKind() == CellKind::DataViewKind;
  }

  static CallResult<HermesValue> create(
      Runtime *runtime,
      Handle<JSObject> prototype);

  /// Retrieves a pointer to the held buffer.
  Handle<JSArrayBuffer> getBuffer(Runtime *runtime) {
    assert(buffer_ && "Cannot get a null buffer");
    return runtime->makeHandle(buffer_);
  }

  /// \return the number of bytes viewed by this DataView.
  size_type byteLength() const {
    return length_;
  }

  /// \return the position within the buffer that this DataView points at.
  size_type byteOffset() const {
    return offset_;
  }

  /// Get the value stored in the bytes from offset to offset + sizeof(T), in
  /// either little or big endian order.
  /// \p offset The distance (in bytes) from the beginning of the DataView to
  ///   look at.
  /// \p littleEndian If true, then read the bytes in little endian order, else
  ///   read them as big endian order.
  /// \return The value which the bytes requested to view represent, as a type.
  /// \pre attached() must be true
  template <typename T>
  T get(size_type offset, bool littleEndian) const;

  /// Set the value stored in the bytes from offset to offset + sizeof(T), in
  /// either little or big endian order.
  /// \p offset The distance (in bytes) from the beginning of the DataView to
  ///   look at.
  /// \p value The typed value to insert into the underlying storage.
  /// \p littleEndian Whether to write the value as little or big endian.
  /// \pre attached() must be true
  template <typename T>
  void set(size_type offset, T value, bool littleEndian);

  /// Check if the underlying JSArrayBuffer is attached.
  /// \return true iff the JSArrayBuffer being viewed by this JSDataView is
  ///   attached to some storage.
  bool attached() const {
    assert(
        buffer_ &&
        "Cannot call attached() when there is not even a buffer set");
    return buffer_->attached();
  }

  void
  setBuffer(GC *gc, JSArrayBuffer *buffer, size_type offset, size_type length) {
    assert(
        offset + length <= buffer->size() &&
        "A DataView cannot be looking outside of the storage");
    buffer_.set(buffer, gc);
    offset_ = offset;
    length_ = length;
  }

 private:
  friend void DataViewBuildMeta(const GCCell *cell, Metadata::Builder &mb);

  /// buffer_ is the underlying storage of the bytes for a DataView.
  GCPointer<JSArrayBuffer> buffer_;
  /// offset_ is the position within the buffer that the DataView begins at.
  size_type offset_;
  /// length_ is the amount of bytes the DataView views inside the storage.
  size_type length_;

  JSDataView(
      Runtime *runtime,
      JSObject *proto,
      HiddenClass *clazz,
      JSObjectPropStorage *propStorage);
};

/// @name Implementations
/// @{

template <typename T>
T JSDataView::get(JSDataView::size_type offset, bool littleEndian) const {
  assert(attached() && "Cannot get on a detached buffer");
  assert(
      offset + sizeof(T) <= length_ &&
      "Trying to read past the end of the storage");
  T result;
  ::memcpy(&result, buffer_->getDataBlock() + offset_ + offset, sizeof(T));
  return llvm::support::endian::byte_swap(
      result,
      littleEndian ? llvm::support::endianness::little
                   : llvm::support::endianness::big);
}

template <typename T>
void JSDataView::set(JSDataView::size_type offset, T value, bool littleEndian) {
  assert(attached() && "Cannot set on a detached buffer");
  assert(
      offset + sizeof(T) <= length_ &&
      "Trying to write past the end of the storage");
  value = llvm::support::endian::byte_swap(
      value,
      littleEndian ? llvm::support::endianness::little
                   : llvm::support::endianness::big);
  memcpy(buffer_->getDataBlock() + offset_ + offset, &value, sizeof(T));
}

/// @}

} // namespace vm
} // namespace hermes

#endif
