#ifndef PROJECT_PROPERTYCACHE_H
#define PROJECT_PROPERTYCACHE_H

#include "hermes/VM/SymbolID.h"

namespace hermes {
namespace vm {
using SlotIndex = uint32_t;

class HiddenClass;

/// A cache entry for a property lookup.
/// If the class operation that we are performing
/// matches the values in the cache entry, \c slot is the index of a
/// non-accessor property.
struct PropertyCacheEntry {
  /// Cached class.
  HiddenClass *clazz{nullptr};

  /// Cached property index.
  SlotIndex slot{0};
};

} // namespace vm
} // namespace hermes
#endif // PROJECT_PROPERTYCACHE_H
