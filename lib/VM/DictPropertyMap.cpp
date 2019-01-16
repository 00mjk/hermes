#define DEBUG_TYPE "vm"
#include "hermes/VM/DictPropertyMap.h"
#include "hermes/Support/Statistic.h"

HERMES_SLOW_STATISTIC(NumDictLookups, "Number of dictionary lookups");
HERMES_SLOW_STATISTIC(NumExtraHashProbes, "Number of extra hash probes");

namespace hermes {
namespace vm {

VTable DictPropertyMap::vt{CellKind::DictPropertyMapKind, 0};

void DictPropertyMapBuildMeta(const GCCell *cell, Metadata::Builder &mb) {
  const auto *self = static_cast<const DictPropertyMap *>(cell);
  mb.addArray<Metadata::ArrayData::ArrayType::Symbol>(
      self->getDescriptorPairs(),
      &self->numDescriptors_,
      sizeof(DictPropertyMap::DescriptorPair));
}

std::pair<bool, DictPropertyMap::HashPair *> DictPropertyMap::lookupEntryFor(
    DictPropertyMap *self,
    SymbolID symbolID) {
  ++NumDictLookups;

  size_type const mask = self->hashCapacity_ - 1;
  size_type index = hash(symbolID) & mask;

  // Probing step.
  size_type step = 1;
  // Save the address of the start of the table to avoid recalculating it.
  HashPair *const tableStart = self->getHashPairs();
  // The first deleted entry we found.
  HashPair *deleted = nullptr;

  assert(symbolID.isValid() && "looking for an invalid SymbolID");

  for (;;) {
    HashPair *curEntry = tableStart + index;

    // Did we find it?
    if (curEntry->first == symbolID)
      return {true, curEntry};

    // If we encountered an empty pair, the search is over - we failed.
    // Return either this entry or a deleted one, if we encountered one.
    if (curEntry->first == ReservedSymbolID::empty)
      return {false, deleted ? deleted : curEntry};

    // The first time we encounter a deleted entry, record it so we can
    // potentially reuse it for insertion.
    if (curEntry->first == ReservedSymbolID::deleted && !deleted)
      deleted = curEntry;

    ++NumExtraHashProbes;
    index = (index + step) & mask;
    ++step;
  }
}

void DictPropertyMap::grow(
    MutableHandle<DictPropertyMap> &selfHandleRef,
    Runtime *runtime,
    size_type newCapacity) {
  auto *newSelf = create(runtime, newCapacity).get();
  auto *self = *selfHandleRef;

  selfHandleRef = newSelf;

  auto *dst = newSelf->getDescriptorPairs();
  size_type count = 0;

  for (auto *src = self->getDescriptorPairs(), *e = src + self->numDescriptors_;
       src != e;
       ++src) {
    if (src->first.isInvalid())
      continue;

    auto key = src->first;

    dst->first = key;
    dst->second = src->second;

    auto result = lookupEntryFor(newSelf, key);
    assert(!result.first && "found duplicate entry while growing");
    result.second->first = key;
    result.second->second = count;

    ++dst;
    ++count;
  }

  assert(
      count == self->numProperties_ && "numProperties mismatch when growing");

  newSelf->numProperties_ = count;

  // Transfer the deleted list to the new instance.
  auto deletedIndex = self->deletedListHead_;
  if (deletedIndex != END_OF_LIST) {
    newSelf->deletedListHead_ = count;
    newSelf->deletedListSize_ = self->deletedListSize_;

    do {
      const auto *src = self->getDescriptorPairs() + deletedIndex;
      assert(
          src->first == ReservedSymbolID::deleted &&
          "pair in the deleted list is not marked as deleted");

      dst->first = ReservedSymbolID::deleted;
      dst->second.slot = src->second.slot;

      deletedIndex = getNextDeletedIndex(src);
      setNextDeletedIndex(
          dst, deletedIndex != END_OF_LIST ? count + 1 : END_OF_LIST);

      ++dst;
      ++count;
    } while (deletedIndex != END_OF_LIST);
  }

  newSelf->numDescriptors_ = count;
  assert(count <= newSelf->descriptorCapacity_);
}

std::pair<NamedPropertyDescriptor *, bool> DictPropertyMap::findOrAdd(
    MutableHandle<DictPropertyMap> &selfHandleRef,
    Runtime *runtime,
    SymbolID id) {
  auto *self = *selfHandleRef;
  auto found = lookupEntryFor(self, id);
  if (found.first) {
    return {&self->getDescriptorPairs()[found.second->second].second, false};
  }

  // We want to grow the hash table if the number of occupied hash entries
  // exceeds 75% of capacity or if the descriptor array is full. Since the
  // capacity of the table is 4/3 of the capacity of the descriptor array, it is
  // sufficient to only check for the latter.

  if (self->numDescriptors_ == self->descriptorCapacity_) {
    grow(
        selfHandleRef,
        runtime,
        self->numProperties_ == self->descriptorCapacity_
            ? self->numProperties_ * 2
            : self->numProperties_ + 1 + self->deletedListSize_);

    self = *selfHandleRef;

    found = lookupEntryFor(self, id);
  }

  ++self->numProperties_;
  if (found.second->first == ReservedSymbolID::deleted)
    self->decDeletedHashCount();

  found.second->first = id;
  found.second->second = self->numDescriptors_;

  auto *descPair = self->getDescriptorPairs() + self->numDescriptors_;

  descPair->first = id;
  ++self->numDescriptors_;

  return {&descPair->second, true};
}

void DictPropertyMap::erase(DictPropertyMap *self, PropertyPos pos) {
  auto *hashPair = self->getHashPairs() + pos.hashPairIndex;
  assert(hashPair->first.isValid() && "erasing invalid property");

  auto descIndex = hashPair->second;
  assert(descIndex < self->numDescriptors_ && "descriptor index out of range");

  auto *descPair = self->getDescriptorPairs() + descIndex;
  assert(
      descPair->first != ReservedSymbolID::empty &&
      "accessing deleted descriptor pair");

  hashPair->first = ReservedSymbolID::deleted;
  descPair->first = ReservedSymbolID::deleted;
  // Add the descriptor to the deleted list.
  setNextDeletedIndex(descPair, self->deletedListHead_);
  self->deletedListHead_ = descIndex;
  ++self->deletedListSize_;

  assert(self->numProperties_ != 0 && "num properties out of sync");
  --self->numProperties_;
  self->incDeletedHashCount();
}

SlotIndex DictPropertyMap::allocatePropertySlot(DictPropertyMap *self) {
  // If there are no deleted properties, the number of properties corresponds
  // exactly to the number of slots.
  if (self->deletedListHead_ == END_OF_LIST)
    return self->numProperties_;

  auto *deletedPair = self->getDescriptorPairs() + self->deletedListHead_;
  assert(
      deletedPair->first == ReservedSymbolID::deleted &&
      "Head of deleted list is not deleted");

  // Remove the first element from the deleted list.
  self->deletedListHead_ = getNextDeletedIndex(deletedPair);
  --self->deletedListSize_;

  // Mark the pair as "invalid" instead of "deleted".
  deletedPair->first = ReservedSymbolID::empty;

  return deletedPair->second.slot;
}

void DictPropertyMap::dump() {
  auto &OS = llvm::errs();

  OS << "DictPropertyMap:" << getDebugAllocationId() << "\n";
  OS << "  HashPairs[" << hashCapacity_ << "]:\n";
  for (unsigned i = 0; i < hashCapacity_; ++i) {
    auto *pair = getHashPairs() + i;
    OS << "    (" << pair->first << ", " << pair->second << ")\n";
  }
  OS << "  Descriptors[" << descriptorCapacity_ << "]:\n";
  for (unsigned i = 0; i < descriptorCapacity_; ++i) {
    auto *pair = getDescriptorPairs() + i;
    OS << "    (" << pair->first << ", "
       << "(slot=" << pair->second.slot << "))\n";
  }
}

} // namespace vm
} // namespace hermes
