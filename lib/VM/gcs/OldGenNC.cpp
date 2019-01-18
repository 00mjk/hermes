/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the LICENSE
 * file in the root directory of this source tree.
 */
#define DEBUG_TYPE "gc"
#include "hermes/VM/OldGenNC.h"

#include "hermes/Support/OSCompat.h"
#include "hermes/VM/AdviseUnused.h"
#include "hermes/VM/AllocResult.h"
#include "hermes/VM/CompactionResult-inline.h"
#include "hermes/VM/CompleteMarkState-inline.h"
#include "hermes/VM/GC.h"
#include "hermes/VM/GCBase-inline.h"
#include "hermes/VM/GCPointer-inline.h"
#include "hermes/VM/YoungGenNC-inline.h"

#include "llvm/Support/Debug.h"
#include "llvm/Support/MathExtras.h"

#include <algorithm>
#include <iterator>
#include <vector>

namespace hermes {
namespace vm {

#ifdef HERMES_SLOW_DEBUG
/// These constants allow us to selective turn off some of the
/// slow debugging checks (by modifying this code and recompiling).
/// But they're on by default.
static bool kVerifyCardTableBoundaries = true;
static bool kVerifyCardTable = true;
#endif

/* static */ const char *OldGen::kSegmentName = "hermes-oldgen-segment";

OldGen::OldGen(GenGC *gc, size_t minSize, size_t maxSize, bool releaseUnused)
    : GCGeneration(gc),
      // The minimum old generation size is 2 pages.
      // Round up the minSize as needed.
      minSize_(adjustSizeWithBounds(
          minSize,
          2 * hermes::oscompat::page_size(),
          std::numeric_limits<size_t>::max())),
      // Round up the maxSize as needed.
      maxSize_(adjustSizeWithBounds(
          maxSize,
          2 * hermes::oscompat::page_size(),
          std::numeric_limits<size_t>::max())),
      releaseUnused_(releaseUnused) {
  exchangeActiveSegment(
      {AlignedStorage{&gc_->storageProvider_, kSegmentName}, this});
  if (!activeSegment())
    gc_->oom();

  // Record the initial level, as if we had done a GC before starting.
  didFinishGC();
  updateCardTableBoundary();
}

size_t OldGen::available() const {
  assert(size() >= levelOffset());

  size_t avail = size() - levelOffset();
  size_t trail = trailingExternalMemory();

  // Now delete the external memory.
  return trail <= avail ? avail - trail : 0;
}

void OldGen::growTo(size_t desired) {
  assert(desired == adjustSize(desired) && "Size must be adjusted.");

  if (size() >= desired) {
    return;
  }

  // We only need to grow from a non-segment-aligned size if the current size is
  // less than a segment's worth.
  if (size() < AlignedHeapSegment::maxSize()) {
    assert(filledSegments_.empty());
    activeSegment().growTo(std::min(desired, AlignedHeapSegment::maxSize()));
  }

  size_ = desired;

  // Now update the effective end to the new correct value.
  updateEffectiveEndForExternalMemory();
}

void OldGen::shrinkTo(size_t desired) {
  assert(desired >= used());
  // Note that this assertion implies that desired >= minSize_.
  assert(desired == adjustSize(desired) && "Size must be adjusted.");

  if (size() <= desired) {
    return;
  }

  // We only shrink to a non-segment-aligned size if the desired size is less
  // than a segment's worth.
  if (desired < AlignedHeapSegment::maxSize()) {
    // We should be justified in asserting that there are no filled segments
    // because we checked earlier that we are using less than the desired size
    // so by transitivity, we are using less than a segment's worth of space.
    assert(filledSegments_.empty());
    activeSegment().shrinkTo(desired);
  }

  size_ = desired;

  // Now update the effective end to the new correct value.
  updateEffectiveEndForExternalMemory();
}

bool OldGen::growToFit(size_t amount) {
  size_t unavailable = levelOffset() + trailingExternalMemory();
  size_t adjusted = adjustSize(unavailable + amount);

  // Insufficient space?
  if (adjusted < unavailable + amount) {
    return false;
  }

  // Could not allocate segments to back growth.
  if (!seedSegmentCacheForSize(levelOffset() + amount)) {
    return false;
  }

  growTo(adjusted);
  return true;
}

gcheapsize_t OldGen::bytesAllocatedSinceLastGC() const {
  assert(ownsAllocContext() && "Only called when the context is owned.");
  auto segs = segmentsSinceLastGC();
  assert(segs.begin() != segs.end());

  auto segIt = segs.begin();
  assert(segIt->dbgContainsLevel(levelAtEndOfLastGC_.ptr));

  // First do the diff for the alloc segment at the time of last GC.
  gcheapsize_t res = segIt->level() - levelAtEndOfLastGC_.ptr;

  // Now add any later segments into which allocation has occurred.
  for (++segIt; segIt != segs.end(); ++segIt) {
    res += segIt->used();
  }

  return res;
}

void OldGen::forAllObjs(const std::function<void(GCCell *)> &callback) {
  for (auto &segment : usedSegments()) {
    segment.forAllObjs(callback);
  }
}

#ifndef NDEBUG
bool OldGen::dbgContains(const void *p) const {
  return gc_->dbgContains(p) && !gc_->youngGen_.dbgContains(p);
}

void OldGen::forObjsAllocatedSinceGC(
    const std::function<void(GCCell *)> &callback) {
  auto segs = segmentsSinceLastGC();
  assert(segs.begin() != segs.end());

  auto segIt = segs.begin();
  assert(segIt->dbgContainsLevel(levelAtEndOfLastGC_.ptr));

  // First do the remainder, if any, of the region containing the first such
  // object.
  segIt->forObjsInRange(callback, levelAtEndOfLastGC_.ptr, segIt->level());

  // Now do any subsequent segments.
  for (++segIt; segIt != segs.end(); ++segIt) {
    segIt->forAllObjs(callback);
  }
}
#endif // !NDEBUG

void OldGen::creditExternalMemory(uint32_t size) {
  GCGeneration::creditExternalMemory(size);
  updateEffectiveEndForExternalMemory();
}

void OldGen::debitExternalMemory(uint32_t size) {
  GCGeneration::debitExternalMemory(size);
  updateEffectiveEndForExternalMemory();
}

bool OldGen::ensureFits(size_t amount) {
  if (amount > available()) {
    return false;
  }

  return seedSegmentCacheForSize(levelOffset() + amount);
}

size_t OldGen::effectiveSize() {
  size_t trailingExternalMem = trailingExternalMemory();
  if (trailingExternalMem > size()) {
    return 0;
  } else {
    return size() - trailingExternalMem;
  }
}

OldGen::Location OldGen::effectiveEnd() {
  const size_t offset = effectiveSize();
  const size_t segNum = offset / AlignedHeapSegment::maxSize();
  const size_t segOff = offset % AlignedHeapSegment::maxSize();

  // In the common case, the effective end will be nowhere near the used portion
  // of the heap.
  if (LLVM_LIKELY(segNum > filledSegments_.size())) {
    return Location();
  }

  // In the not so common case, we hope that even if the effective end is close
  // to the level, it is still above it.
  if (LLVM_LIKELY(segNum == filledSegments_.size())) {
    return Location(segNum, trueActiveSegment().start() + segOff);
  }

  // Pathologically bad case -- the effective end lies before the level and the
  // heap is overcommitted.
  return Location(segNum, filledSegments_[segNum].start() + segOff);
}

size_t OldGen::trailingExternalMemory() const {
  // We consider the externalMemory_ to "fill up" fragmentation losses in
  // filled segments -- thus, we subtract such those losses from the external
  // memory for purposes of computing the effective size.  Only the remainder
  // is considered to be "allocated at the end".
  size_t fragLoss = fragmentationLoss();
  if (fragLoss > externalMemory_) {
    return 0;
  } else {
    return externalMemory_ - fragLoss;
  }
}

void OldGen::updateEffectiveEndForExternalMemory() {
  Location desiredEnd = effectiveEnd();

  if (LLVM_LIKELY(!desiredEnd)) {
    // The effective size is not in a used segment, so in particular, it cannot
    // be in the active segment.
    trueActiveSegment().clearExternalMemoryCharge();
    return;
  }

  auto clampedEnd = std::max(level(), desiredEnd);
  assert(
      clampedEnd.segmentNum == filledSegments_.size() &&
      "Effective end should be in the active segment.");

  trueActiveSegment().setEffectiveEnd(clampedEnd.ptr);
}

void OldGen::markYoungGenPointers(OldGen::Location originalLevel) {
  if (used() == 0) {
    // Nothing to do if the old gen is empty.
    return;
  }

#ifdef HERMES_SLOW_DEBUG
  struct VerifyCardDirtyAcceptor final : public SlotAcceptorDefault {
    using SlotAcceptorDefault::accept;
    using SlotAcceptorDefault::SlotAcceptorDefault;

    void accept(void *&ptr) {
      char *valuePtr = reinterpret_cast<char *>(ptr);
      char *locPtr = reinterpret_cast<char *>(&ptr);

      if (gc.youngGen_.contains(valuePtr)) {
        assert(AlignedHeapSegment::cardTableCovering(locPtr)
                   ->isCardForAddressDirty(locPtr));
      }
    }
    void accept(HermesValue &hv) {
      if (!hv.isPointer()) {
        return;
      }

      char *valuePtr = reinterpret_cast<char *>(hv.getPointer());
      char *locPtr = reinterpret_cast<char *>(&hv);

      if (gc.youngGen_.contains(valuePtr)) {
        assert(AlignedHeapSegment::cardTableCovering(locPtr)
                   ->isCardForAddressDirty(locPtr));
      }
    }
  };

  if (kVerifyCardTable) {
    VerifyCardDirtyAcceptor acceptor(*gc_);
    GenGC *gc = gc_;
    for (auto &segment : usedSegments()) {
      segment.forAllObjs([gc, &acceptor](GCCell *cell) {
        GCBase::markCell(cell, gc, acceptor);
      });
    }
  }

  verifyCardTableBoundaries();
#endif // HERMES_SLOW_DEBUG

  struct OldGenObjEvacAcceptor final : public SlotAcceptorDefault {
    using SlotAcceptorDefault::accept;
    using SlotAcceptorDefault::SlotAcceptorDefault;

    // NOTE: C++ does not allow templates on local classes, so duplicate the
    // body of \c helper for ensureReferentCopied.
    void helper(GCCell **slotAddr, void *slotContents) {
      if (gc.youngGen_.contains(slotContents)) {
        gc.youngGen_.ensureReferentCopied(slotAddr);
      }
    }
    void helper(HermesValue *slotAddr, void *slotContents) {
      if (gc.youngGen_.contains(slotContents)) {
        gc.youngGen_.ensureReferentCopied(slotAddr);
      }
    }

    void accept(void *&ptr) {
      helper(reinterpret_cast<GCCell **>(&ptr), ptr);
    }
    void accept(HermesValue &hv) {
      if (hv.isPointer()) {
        helper(&hv, hv.getPointer());
      }
    }
  };

  OldGenObjEvacAcceptor acceptor(*gc_);
  SlotVisitor<OldGenObjEvacAcceptor> visitor(acceptor);

  const auto segLast = segmentIt(originalLevel.segmentNum);
  const auto segEnd = segLast + 1;

  for (auto segIt = segmentIt(0); segIt != segEnd; ++segIt) {
    const char *const origSegLevel =
        segIt == segLast ? originalLevel.ptr : segIt->level();

    auto &cardTable = segIt->cardTable();

    size_t from = cardTable.addressToIndex(segIt->start());
    size_t to = cardTable.addressToIndex(origSegLevel - 1) + 1;

    while (const auto oiBegin = cardTable.findNextDirtyCard(from, to)) {
      const auto iBegin = *oiBegin;

      const auto oiEnd = cardTable.findNextCleanCard(iBegin, to);
      const auto iEnd = oiEnd ? *oiEnd : to;

      assert(
          (iEnd == to || !cardTable.isCardForIndexDirty(iEnd)) &&
          cardTable.isCardForIndexDirty(iEnd - 1) &&
          "end should either be the end of the card table, or the first "
          "non-dirty card after a sequence of dirty cards");
      assert(iBegin < iEnd && "Indices must be apart by at least one");

      const char *const begin = cardTable.indexToAddress(iBegin);
      const char *const end = cardTable.indexToAddress(iEnd);
      const void *const boundary = std::min(end, origSegLevel);

      GCCell *const firstObj = segIt->cardTable().firstObjForCard(iBegin);

      GCCell *obj = firstObj;

      // Mark the first object with respect to the dirty card boundaries.
      GCBase::markCellWithinRange(visitor, obj, obj->getVT(), gc_, begin, end);

      // Mark the objects that are entirely contained within the dirty card
      // boundaries.
      for (GCCell *next = obj->nextCell(); next < boundary;
           next = next->nextCell()) {
        obj = next;
        GCBase::markCell(visitor, obj, obj->getVT(), gc_);
      }

      // Mark the final object in the range with respect to the dirty card
      // boundaries, as long as it does not coincide with the first object.
      if (LLVM_LIKELY(obj != firstObj)) {
        GCBase::markCellWithinRange(
            visitor, obj, obj->getVT(), gc_, begin, end);
      }

      from = iEnd;
    }
    cardTable.clear();
  }
}

void OldGen::youngGenTransitiveClosure(
    const Location &toScanLoc,
    YoungGen::EvacAcceptor &acceptor) {
  size_t toScanSegmentNum = toScanLoc.segmentNum;
  char *toScanPtr = toScanLoc.ptr;

  // Predicate to check whether the the index \p ix refers to a segment that has
  // already been "filled", which implies that its level will not change.
  const auto isFilled = [this](size_t ix) {
    return ix < filledSegments_.size();
  };

  // We must scan until the to-scan segment number and pointer reach the current
  // allocation point.  This loop nest is maximally specialized for
  // performance, see T26274987 for details.
  while (isFilled(toScanSegmentNum) || toScanPtr < activeSegment().level()) {
    // Now we have two interior loops: we can be faster for already
    // filled segments, since their levels won't change.
    while (isFilled(toScanSegmentNum)) {
      AlignedHeapSegment &curSegment = filledSegments_[toScanSegmentNum];
      const char *const curSegmentLevel = curSegment.level();
      while (toScanPtr < curSegmentLevel) {
        GCCell *cell = reinterpret_cast<GCCell *>(toScanPtr);
        toScanPtr += cell->getAllocatedSize();
        // Ask the object to mark the pointers it owns.
        GCBase::markCell(cell, gc_, acceptor);
      }
      toScanSegmentNum++;
      toScanPtr = isFilled(toScanSegmentNum)
          ? filledSegments_[toScanSegmentNum].start()
          : activeSegment().start();
    }

    // We should have reached the current allocation segment.
    assert(toScanSegmentNum == filledSegments_.size());
    assert(activeSegment().dbgContainsLevel(toScanPtr));
    const char *const activeLowLim = activeSegment().lowLim();

    // Now the level of the region can change as we call markCell, so
    // we must query each time.
    while (activeLowLim == activeSegment().lowLim() &&
           toScanPtr < activeSegment().level()) {
      const char *const activeLevel = activeSegment().level();
      while (toScanPtr < activeLevel) {
        GCCell *cell = reinterpret_cast<GCCell *>(toScanPtr);
        toScanPtr += cell->getAllocatedSize();
        // Ask the object to mark the pointers it owns.
        GCBase::markCell(cell, gc_, acceptor);
      }
    }

    // We're not necessarily done; allocation may have moved to a
    // later segment.  So we go around the loop again to make sure we
    // reach the proper termination condition.
  }
}

#ifdef HERMES_SLOW_DEBUG
void OldGen::verifyCardTableBoundaries() const {
  if (kVerifyCardTableBoundaries) {
    for (const auto &segment : usedSegments()) {
      segment.cardTable().verifyBoundaries(segment.start(), segment.level());
    }
  }
}
#endif

void OldGen::sweepAndInstallForwardingPointers(
    GC *gc,
    SweepResult *sweepResult) {
  for (auto &segment : usedSegments()) {
    segment.sweepAndInstallForwardingPointers(gc, sweepResult);
  }
}

void OldGen::updateReferences(GC *gc, SweepResult::VTablesRemaining &vTables) {
  auto acceptor = getFullMSCUpdateAcceptor(*gc);
  for (auto &segment : usedSegments()) {
    segment.updateReferences(gc, acceptor.get(), vTables);
  }
  updateFinalizableCellListReferences();
}

void OldGen::recordLevelAfterCompaction(
    CompactionResult::ChunksRemaining &chunks) {
  auto segs = usedSegments();
  auto segIt = segs.begin();

  using std::distance;
  const size_t nSegs = distance(segIt, segs.end());
  const size_t nChunks = chunks.size();
  const auto segEnd = segIt + std::min(nSegs, nChunks);

  assert(nSegs > 0 && "Segment list cannot be empty");

  usedInFilledSegments_ = 0;
  size_t usedInPrev = 0;

  // Some prefix of the used chunks correspond to segments in this generation.
  // The corresponding segments are the used segments after compaction.
  for (; segIt != segEnd && this == chunks.peek().generation(); ++segIt) {
    if (releaseUnused_)
      chunks.next().recordLevel<AdviseUnused::Yes>(&*segIt);
    else
      chunks.next().recordLevel<AdviseUnused::No>(&*segIt);
    usedInFilledSegments_ += usedInPrev;
    usedInPrev = segIt->used();
  }

  auto usedSegs = distance(segs.begin(), segIt);
  releaseSegments(usedSegs == 0 ? 1 : usedSegs);
  updateCardTableBoundary();
}

AllocResult OldGen::fullCollectThenAlloc(
    uint32_t allocSize,
    HasFinalizer hasFinalizer) {
  gc_->collect(/* canEffectiveOOM */ true);
  {
    AllocResult res = allocRaw(allocSize, hasFinalizer);
    if (LLVM_LIKELY(res.success)) {
      return res;
    }
  }

  if (growToFit(allocSize)) {
    AllocResult res = allocRaw(allocSize, hasFinalizer);
    assert(res.success && "preceding test should guarantee success.");
    return res;
  }

  gc_->oom();
}

void OldGen::moveHeap(GC *gc, ptrdiff_t moveHeapDelta) {
  // TODO (T25686322): implement non-contig version of this.
}

void OldGen::updateCardTablesAfterCompaction(bool youngIsEmpty) {
  for (auto &segment : usedSegments()) {
    if (youngIsEmpty) {
      segment.cardTable().clear();
    } else {
      segment.cardTable().updateAfterCompaction(segment.level());
    }
  }

#ifdef HERMES_SLOW_DEBUG
  verifyCardTableBoundaries();
#endif
}

void OldGen::recreateCardTableBoundaries() {
  for (auto &segment : usedSegments()) {
    segment.recreateCardTableBoundaries();
  }

  updateCardTableBoundary();
#ifdef HERMES_SLOW_DEBUG
  verifyCardTableBoundaries();
#endif
}

size_t OldGen::maxSegments() const {
  return segmentsForSize(maxSize_);
}

bool OldGen::seedSegmentCacheForSize(size_t size) {
  auto committedSegs = [this]() {
    return filledSegments_.size() + segmentCache_.size() + 1;
  };

  const auto segReq = segmentsForSize(size);
  auto segAlloc = committedSegs();

  // Remember how many segments we had cached in case we need to rollback.
  const auto cacheBefore = segmentCache_.size();

  // Try and seed the segment cache with enough segments to fit the request.
  for (; segAlloc < segReq; ++segAlloc) {
    segmentCache_.emplace_back(
        AlignedStorage{&gc_->storageProvider_, kSegmentName}, this);

    if (!initSegmentForMaterialization(segmentCache_.back())) {
      // We could not allocate all the segments we needed, so give back the ones
      // we were able to allocate.
      segmentCache_.resize(cacheBefore);
      return false;
    }
  }

  assert(committedSegs() >= segReq);
  return true;
}

bool OldGen::materializeNextSegment() {
  auto usedSegs = filledSegments_.size() + 1;
  auto maxSizeSegs = segmentsForSize(effectiveSize());

  if (usedSegs >= maxSizeSegs) {
    return false;
  }

  // Save the number of used bytes in the currently active segment and TLAB
  const size_t usedInFilledSeg = activeSegment().used();

  // Reserve a slot for the previously active segment
  filledSegments_.emplace_back();
  AlignedHeapSegment *filledSegSlot = &filledSegments_.back();

  // Get a new segment from somewhere
  if (!segmentCache_.empty()) {
    exchangeActiveSegment(std::move(segmentCache_.back()), filledSegSlot);
    segmentCache_.pop_back();
  } else {
    exchangeActiveSegment(
        {AlignedStorage{&gc_->storageProvider_, kSegmentName}, this},
        filledSegSlot);
    bool initSuccess = initSegmentForMaterialization(activeSegmentRaw());
    if (LLVM_UNLIKELY(!initSuccess)) {
      exchangeActiveSegment(std::move(*filledSegSlot));
      filledSegments_.pop_back();
      return false;
    }
  }

  // The active segment has changed, so we need to update the next card table
  // boundary to align with the start of its allocation region.
  updateCardTableBoundary();
  usedInFilledSegments_ += usedInFilledSeg;
  filledSegSlot->clearExternalMemoryCharge();

  // We may have moved into the segment containing the effective end. Also,
  // declaring the segment full may leave a portion unallocated; we consider
  // external memory as if it fills in such fragmentation losses.  So update
  // the effective end of the generation.
  updateEffectiveEndForExternalMemory();

  return true;
}

bool OldGen::initSegmentForMaterialization(AlignedHeapSegment &segment) {
  if (!segment) {
    return false;
  }

  segment.growToLimit();
  return true;
}

void OldGen::releaseSegments(size_t from) {
  assert(from > 0 && "Cannot release every segment.");

  // TODO (T30523258) Experiment with different schemes for deciding
  // how many segments we keep in the cache, trading off the VA cost,
  // against the cost of allocating a fresh segment.
  if (releaseUnused_)
    segmentCache_.clear();

  const auto nSegs = filledSegments_.size() + 1;
  if (from >= nSegs) {
    // Nothing more to do
    return;
  }

  std::vector<const char *> toRelease;
  const auto release = [&toRelease, this](AlignedHeapSegment &unused) {
    toRelease.push_back(unused.lowLim());
    if (!this->releaseUnused_) {
      // Clear the segment and move it to the cache.
      unused.resetLevel();
      assert(unused.used() == 0);
      this->segmentCache_.emplace_back(std::move(unused));
    }
  };

  // Release the current active segment first
  release(activeSegment());

  auto first = filledSegments_.begin() + from;
  auto last = filledSegments_.end();

  std::for_each(first, last, release);

  std::sort(toRelease.begin(), toRelease.end());
  gc_->forgetSegments(toRelease);

  // The new active segment was the last unreleased one.
  exchangeActiveSegment(std::move(filledSegments_[from - 1]));
  filledSegments_.resize(from - 1);
}

AllocResult OldGen::allocSlow(uint32_t size, HasFinalizer hasFinalizer) {
  assert(ownsAllocContext());
  AllocResult result = allocRawSlow(size, hasFinalizer);
  if (LLVM_LIKELY(result.success)) {
    return result;
  }
  return fullCollectThenAlloc(size, hasFinalizer);
}

void OldGen::updateCardTableBoundary() {
  assert(ownsAllocContext());
  cardBoundary_ =
      activeSegment().cardTable().nextBoundary(activeSegment().level());
}

AllocResult OldGen::allocRawSlow(uint32_t size, HasFinalizer hasFinalizer) {
  assert(ownsAllocContext() && "Only called when the context is owned.");
  // The size being allocated must fit in a segment.
  if (LLVM_UNLIKELY(size > AlignedHeapSegment::maxSize())) {
    gc_->oom();
  }

  // Allocation failed in the current segment; try the next one, if
  // possible.  Are there more segments to try?
  if (LLVM_UNLIKELY(!materializeNextSegment())) {
    return {nullptr, false};
  }

  // This looks like potentially unbounded recursion, but it is not.  We
  // asserted above that the the size being allocated fits in a fully empty
  // segment.  At this point we have successfully materialized a new empty, and
  // max sized segment, so the allocation is guaranteed to succeed.
  return allocRaw(size, hasFinalizer);
}

#ifdef HERMES_SLOW_DEBUG
void OldGen::checkWellFormed(const GC *gc) const {
  uint64_t totalExtSize = 0;
  for (auto &segment : usedSegments()) {
    uint64_t extSize = 0;
    segment.checkWellFormed(gc, &extSize);
    totalExtSize += extSize;
  }
  assert(totalExtSize == externalMemory());
  checkFinalizableObjectsListWellFormed();
}
#endif

void OldGen::didFinishGC() {
  levelAtEndOfLastGC_ = levelDirect();
}

} // namespace vm
} // namespace hermes
