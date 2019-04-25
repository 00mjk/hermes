/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the LICENSE
 * file in the root directory of this source tree.
 */
#ifndef HERMES_BCGEN_HBC_UNIQUINGSTRINGLITERALTABLE_H
#define HERMES_BCGEN_HBC_UNIQUINGSTRINGLITERALTABLE_H

#include "hermes/BCGen/HBC/ConsecutiveStringStorage.h"

namespace hermes {
namespace hbc {

/// Implementation shared between the UniquingStringLiteralAccumulator, which
/// gathers strings into a storage, and the UniquingStringLiteralTable, which
/// exposes the mapping from string to numeric ID.
struct StringLiteralIDMapping {
  /// \return true if and only if no strings have been recorded.
  inline bool empty() const;

 protected:
  StringLiteralIDMapping() = default;

  /// Decode the strings from \p css to seed the mapping.
  explicit StringLiteralIDMapping(const ConsecutiveStringStorage &css);

  /// Mapping between strings and IDs.
  StringSetVector strings_;

  // Mapping such that \c isIdentifier_[i] is true if and only if the string at
  // \c strings_[i] should be treated as an identifier.
  std::vector<bool> isIdentifier_;
};

/// Gathers strings into a storage. Because the indices of strings in the
/// resulting storage may change between when the string is added and when the
/// storage is created, this class does not return the ID of the string when it
/// is added.
class UniquingStringLiteralAccumulator final : public StringLiteralIDMapping {
  /// Strings the accumulator was initialised with.
  ConsecutiveStringStorage storage_;

  /// The number of times a string was added as an identifier.  This information
  /// is only tracked for newly added strings (those not in the storage the
  /// accumulator may have been initialized with) and is keyed by the offset of
  /// the string in the mapping from the first newly added string.  This
  /// information is used to order the resulting storage.
  std::vector<size_t> numIdentifierRefs_;

 public:
  UniquingStringLiteralAccumulator() = default;

  /// Seed the mapping from the strings in \p css and take ownership of it.
  inline explicit UniquingStringLiteralAccumulator(
      ConsecutiveStringStorage css);

  /// Add a new string -- \p str -- to the accumulation.  If \p isIdentifier is
  /// true, then the string is marked as potentially being used as an
  /// identifier.
  inline void addString(llvm::StringRef str, bool isIdentifier);

  /// \return a ConsecutiveStringStorage for \p strings.  If \p optimize is
  /// set, attempt to pack the strings in the storage to reduce their size.
  static ConsecutiveStringStorage toStorage(
      UniquingStringLiteralAccumulator strings,
      bool optimize = false);
};

/// Exposes the mapping from strings to their IDs in a
/// ConsecutiveStringStorage.  This class does not own the storage it is
/// mapping, but also does not require the storage to outlive it.
struct StringLiteralTable final : public StringLiteralIDMapping {
  StringLiteralTable() = default;
  inline StringLiteralTable(const ConsecutiveStringStorage &css);

  /// \return string id of an existing \p str in string table.
  inline uint32_t getStringID(llvm::StringRef str) const;

  /// \return string id of an existing \p str in string table, assuming it is
  /// marked as an identifier.
  inline uint32_t getIdentifierID(llvm::StringRef str) const;
};

inline bool StringLiteralIDMapping::empty() const {
  return strings_.size() == 0;
}

inline UniquingStringLiteralAccumulator::UniquingStringLiteralAccumulator(
    ConsecutiveStringStorage css)
    : StringLiteralIDMapping(css), storage_(std::move(css)) {}

inline void UniquingStringLiteralAccumulator::addString(
    llvm::StringRef str,
    bool isIdentifier) {
  assert(strings_.size() == isIdentifier_.size());
  const auto fresh = strings_.size();
  auto id = strings_.insert(str);
  if (id == fresh) {
    isIdentifier_.push_back(false);
    numIdentifierRefs_.push_back(0);
  }

  if (isIdentifier) {
    isIdentifier_[id] = true;
    if (id >= storage_.count()) {
      // We only track the frequency of new strings, so the ID needs to be
      // translated.
      numIdentifierRefs_[id - storage_.count()]++;
    }
  }
}

inline StringLiteralTable::StringLiteralTable(
    const ConsecutiveStringStorage &css)
    : StringLiteralIDMapping(css) {}

inline uint32_t StringLiteralTable::getStringID(llvm::StringRef str) const {
  auto iter = strings_.find(str);
  assert(
      iter != strings_.end() &&
      "The requested string is not in the mapping.  Is the part of the IR that "
      "introduces it being traversed by one of the functions in "
      "TraverseLiteralStrings.h ?");
  return std::distance(strings_.begin(), iter);
}

inline uint32_t StringLiteralTable::getIdentifierID(llvm::StringRef str) const {
  auto idx = getStringID(str);
  assert(
      isIdentifier_[idx] &&
      "The requested string exists in the mapping but was not marked as an "
      "identifier.  When it was added to the mapping during a call to one "
      "of the traversal functions in TraverseLiteralStrings.h, was the usage "
      "of the string as an identifier correctly traversed?");
  return idx;
}

} // namespace hbc
} // namespace hermes

#endif // HERMES_BCGEN_HBC_UNIQUINGSTRINGLITERALTABLE_H
