#include "llvm/Support/GenericDomTree.h"
#include "llvm/Support/GenericDomTreeConstruction.h"

#include "hermes/IR/CFG.h"

using namespace hermes;

template class llvm::DominatorTreeBase<BasicBlock>;
template class llvm::DomTreeNodeBase<BasicBlock>;

DominanceInfo::DominanceInfo(Function *F)
    : DominatorTreeBase(/*isPostDom*/ false) {
  assert(F->begin() != F->end() && "Function is empty!");
  recalculate<Function>(*F);
}

bool DominanceInfo::properlyDominates(
    const Instruction *A,
    const Instruction *B) const {
  const BasicBlock *ABB = A->getParent();
  const BasicBlock *BBB = B->getParent();

  if (ABB != BBB)
    return properlyDominates(ABB, BBB);

  // Otherwise, they're in the same block, and we just need to check
  // whether B comes after A.
  auto ItA = A->getIterator();
  auto ItB = B->getIterator();
  auto E = ABB->begin();
  while (ItB != E) {
    --ItB;
    if (ItA == ItB)
      return true;
  }

  return false;
}
