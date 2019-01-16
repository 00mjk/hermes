#ifndef HERMES_PUBLIC_BUFFER_H
#define HERMES_PUBLIC_BUFFER_H

#include <cstddef>
#include <cstdint>

namespace hermes {

/// A generic buffer interface.  E.g. for memmapped bytecode.
class Buffer {
 public:
  virtual ~Buffer() {}

  const uint8_t *data() const {
    return data_;
  };

  size_t size() const {
    return size_;
  }

 protected:
  const uint8_t *data_ = nullptr;
  size_t size_ = 0;
};

} // namespace hermes

#endif
