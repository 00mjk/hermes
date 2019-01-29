/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the LICENSE
 * file in the root directory of this source tree.
 */
#include "ESTreeIRGen.h"

namespace hermes {
namespace irgen {

void ESTreeIRGen::genBody(ESTree::NodeList &Body) {
  DEBUG(dbgs() << "Compiling body.\n");

  // Generate code for the declarations statements.
  for (auto &Node : Body) {
    DEBUG(dbgs() << "IRGen node of type " << Node.getNodeName() << ".\n");
    genStatement(&Node);
  }
}

void ESTreeIRGen::genStatement(ESTree::Node *stmt) {
  DEBUG(dbgs() << "IRGen statement of type " << stmt->getNodeName() << "\n");
  IRBuilder::ScopedLocationChange slc(Builder, stmt->getDebugLoc());

  Builder.getFunction()->incrementStatementCount();

  // IRGen Function declarations.
  if (/* auto *FD = */ dyn_cast<ESTree::FunctionDeclarationNode>(stmt)) {
    // It has already been hoisted. Do nothing.  But, keep this to
    // match the AST structure, and we may want to do something in the
    // future.
    return;
  }

  // IRGen if statement.
  if (auto *IF = dyn_cast<ESTree::IfStatementNode>(stmt)) {
    return genIfStatement(IF);
  }

  // IRGen for-in statement.
  if (auto *FIS = dyn_cast<ESTree::ForInStatementNode>(stmt)) {
    return genForInStatement(FIS);
  }

  // IRGen return statement.
  if (auto *Ret = dyn_cast<ESTree::ReturnStatementNode>(stmt)) {
    return genReturnStatement(Ret);
  }

  // Expression statement.
  if (auto *exprStmt = dyn_cast<ESTree::ExpressionStatementNode>(stmt)) {
    return genExpressionWrapper(exprStmt->_expression);
  }

  // Handle Switch expressions.
  if (auto *SW = dyn_cast<ESTree::SwitchStatementNode>(stmt)) {
    return genSwitchStatement(SW);
  }

  // Variable declarations:
  if (auto *VND = dyn_cast<ESTree::VariableDeclaratorNode>(stmt)) {
    auto variableName = getNameFieldFromID(VND->_id);
    DEBUG(
        dbgs() << "IRGen variable declaration for \"" << variableName
               << "\".\n");

    // Materialize the initialization clause and save it into the variable.
    auto *storage = nameTable_.lookup(variableName);
    assert(storage && "Declared variable not found in name table");
    if (VND->_init) {
      DEBUG(dbgs() << "Variable \"" << variableName << "\" has initializer.\n");
      emitStore(Builder, genExpression(VND->_init, variableName), storage);
    }

    return;
  }

  if (auto *VDN = dyn_cast<ESTree::VariableDeclarationNode>(stmt)) {
    for (auto &decl : VDN->_declarations) {
      genStatement(&decl);
    }
    return;
  }

  // IRGen the content of the block.
  if (auto *BS = dyn_cast<ESTree::BlockStatementNode>(stmt)) {
    for (auto &Node : BS->_body) {
      genStatement(&Node);
    }

    return;
  }

  if (auto *Label = dyn_cast<ESTree::LabeledStatementNode>(stmt)) {
    // Create a new basic block which is the continuation of the current block
    // and the jump target of the label.
    BasicBlock *next = Builder.createBasicBlock(functionContext->function);

    // Set the jump point for the label to the new block.
    functionContext->labels[Label->getLabelIndex()].breakTarget = next;

    // Now, generate the IR for the statement that the label is annotating.
    genStatement(Label->_body);

    // End the current basic block with a jump to the new basic block.
    Builder.createBranchInst(next);
    Builder.setInsertionBlock(next);

    return;
  }

  // Handle the call expression that could appear in the context of statement
  // expr without the ExpressionStatementNode wrapper.
  if (auto *call = dyn_cast<ESTree::CallExpressionNode>(stmt)) {
    return genExpressionWrapper(call);
  }

  if (auto *W = dyn_cast<ESTree::WhileStatementNode>(stmt)) {
    DEBUG(dbgs() << "IRGen 'while' statement\n");
    genForWhileLoops(W, nullptr, W->_test, W->_test, nullptr, W->_body);
    return;
  }

  if (auto *F = dyn_cast<ESTree::ForStatementNode>(stmt)) {
    DEBUG(dbgs() << "IRGen 'for' statement\n");
    genForWhileLoops(F, F->_init, F->_test, F->_test, F->_update, F->_body);
    return;
  }

  if (auto *D = dyn_cast<ESTree::DoWhileStatementNode>(stmt)) {
    DEBUG(dbgs() << "IRGen 'do..while' statement\n");
    genForWhileLoops(D, nullptr, nullptr, D->_test, nullptr, D->_body);
    return;
  }

  if (auto *breakStmt = dyn_cast<ESTree::BreakStatementNode>(stmt)) {
    DEBUG(dbgs() << "IRGen 'break' statement\n");

    auto labelIndex = breakStmt->getLabelIndex();
    auto &label = functionContext->labels[labelIndex];
    assert(label.breakTarget && "breakTarget not set");

    genFinallyBeforeControlChange(
        breakStmt->surroundingTry,
        functionContext->getSemInfo()->labels[labelIndex].surroundingTry);
    Builder.createBranchInst(label.breakTarget);

    // Continue code generation for stuff that comes after the break statement
    // in a new dead block.
    auto newBlock = Builder.createBasicBlock(functionContext->function);
    Builder.setInsertionBlock(newBlock);
    return;
  }

  if (auto *continueStmt = dyn_cast<ESTree::ContinueStatementNode>(stmt)) {
    DEBUG(dbgs() << "IRGen 'continue' statement\n");

    auto labelIndex = continueStmt->getLabelIndex();
    auto &label = functionContext->labels[labelIndex];
    assert(label.continueTarget && "continueTarget not set");

    genFinallyBeforeControlChange(
        continueStmt->surroundingTry,
        functionContext->getSemInfo()->labels[labelIndex].surroundingTry);
    Builder.createBranchInst(label.continueTarget);

    // Continue code generation for stuff that comes after the break statement
    // in a new dead block.
    auto newBlock = Builder.createBasicBlock(functionContext->function);
    Builder.setInsertionBlock(newBlock);
    return;
  }

  if (auto *T = dyn_cast<ESTree::TryStatementNode>(stmt)) {
    genTryStatement(T);
    return;
  }

  if (auto *T = dyn_cast<ESTree::ThrowStatementNode>(stmt)) {
    DEBUG(dbgs() << "IRGen 'throw' statement\n");
    Value *rightHandVal = genExpression(T->_argument);
    Builder.createThrowInst(rightHandVal);

    // Throw interferes with control flow, hence we need a new block.
    auto newBlock =
        Builder.createBasicBlock(Builder.getInsertionBlock()->getParent());
    Builder.setInsertionBlock(newBlock);
    return;
  }

  // Handle empty statements.
  if (isa<ESTree::EmptyStatementNode>(stmt)) {
    return;
  }

  // Handle debugger statements.
  if (isa<ESTree::DebuggerStatementNode>(stmt)) {
    Builder.createDebuggerInst();
    return;
  }

  Builder.getModule()->getContext().getSourceErrorManager().error(
      stmt->getSourceRange(), Twine("Unsupported statement encountered."));
}

void ESTreeIRGen::genExpressionWrapper(ESTree::Node *expr) {
  Value *val = genExpression(expr);
  if (functionContext->globalReturnRegister) {
    Builder.createStoreStackInst(val, functionContext->globalReturnRegister);
  }
}

void ESTreeIRGen::genIfStatement(ESTree::IfStatementNode *IfStmt) {
  DEBUG(dbgs() << "IRGen IF-stmt.\n");

  auto Parent = Builder.getInsertionBlock()->getParent();
  auto ThenBlock = Builder.createBasicBlock(Parent);
  auto ElseBlock = Builder.createBasicBlock(Parent);
  auto ContinueBlock = Builder.createBasicBlock(Parent);

  genExpressionBranch(IfStmt->_test, ThenBlock, ElseBlock);

  // IRGen the Then:
  Builder.setInsertionBlock(ThenBlock);
  genStatement(IfStmt->_consequent);
  Builder.createBranchInst(ContinueBlock);

  // IRGen the Else, if it exists:
  Builder.setInsertionBlock(ElseBlock);
  if (IfStmt->_alternate) {
    genStatement(IfStmt->_alternate);
  }

  Builder.createBranchInst(ContinueBlock);
  Builder.setInsertionBlock(ContinueBlock);
}

void ESTreeIRGen::genForWhileLoops(
    ESTree::LoopStatementNode *loop,
    ESTree::Node *init,
    ESTree::Node *preTest,
    ESTree::Node *postTest,
    ESTree::Node *update,
    ESTree::Node *body) {
  /* In this section we generate a sequence of basic blocks that implement
   the for, while and do..while statements. Loop inversion is applied.
   For loops are syntactic-sugar for while
   loops and both have pre-test and post-test. do..while loop should only
   have post-test. They will all have the following structure:

        [ current block  ]
        [      init      ]
        [ pre test block ]
               |       \
               |        \
               |         \      ->[ exit block ]
    /-->  [ body block ]  \____/      ^
    |    [ update block ]             |
    |   [ post test block ]  --------/
    \__________/
  */

  // Create the basic blocks that make the while structure.
  Function *function = Builder.getInsertionBlock()->getParent();
  BasicBlock *bodyBlock = Builder.createBasicBlock(function);
  BasicBlock *exitBlock = Builder.createBasicBlock(function);
  BasicBlock *preTestBlock = Builder.createBasicBlock(function);
  BasicBlock *postTestBlock = Builder.createBasicBlock(function);
  BasicBlock *updateBlock = Builder.createBasicBlock(function);

  // Initialize the goto labels.
  auto &label = functionContext->labels[loop->getLabelIndex()];
  label.breakTarget = exitBlock;
  label.continueTarget = updateBlock;

  // Generate IR for the loop initialization.
  // The init field can be a variable declaration or any expression.
  // https://github.com/estree/estree/blob/master/spec.md#forstatement
  if (init) {
    if (isa<ESTree::VariableDeclarationNode>(init)) {
      genStatement(init);
    } else {
      genExpression(init);
    }
  }

  // Terminate the loop header section and jump to the condition block.
  Builder.createBranchInst(preTestBlock);
  Builder.setInsertionBlock(preTestBlock);

  // Branch out of the loop if the condition is false.
  if (preTest)
    genExpressionBranch(preTest, bodyBlock, exitBlock);
  else
    Builder.createBranchInst(bodyBlock);

  // Generate the update sequence of 'for' loops.
  Builder.setInsertionBlock(updateBlock);
  if (update) {
    genExpression(update);
  }

  // After executing the content of the body, jump to the post test block.
  Builder.createBranchInst(postTestBlock);
  Builder.setInsertionBlock(postTestBlock);

  // Branch out of the loop if the condition is false.
  if (postTest)
    genExpressionBranch(postTest, bodyBlock, exitBlock);
  else
    Builder.createBranchInst(bodyBlock);

  // Now, generate the body of the while loop.
  // Do this last so that the test and update blocks are associated with the
  // loop statement, and not the body statement.
  Builder.setInsertionBlock(bodyBlock);
  genStatement(body);
  Builder.createBranchInst(updateBlock);

  // Following statements are inserted to the exit block.
  Builder.setInsertionBlock(exitBlock);
}

void ESTreeIRGen::genForInStatement(ESTree::ForInStatementNode *ForInStmt) {
  // The state of the enumerator. Notice that the instruction writes to the
  // storage
  // variables just like Load/Store instructions write to stack allocations.
  auto *iteratorStorage =
      Builder.createAllocStackInst(genAnonymousLabelName("iter"));
  auto *baseStorage =
      Builder.createAllocStackInst(genAnonymousLabelName("base"));
  auto *indexStorage =
      Builder.createAllocStackInst(genAnonymousLabelName("idx"));
  auto *sizeStorage =
      Builder.createAllocStackInst(genAnonymousLabelName("size"));

  // Generate the right hand side of the for-in loop. The result of this
  // expression is the object we iterate on. We use this object as the 'base'
  // of the enumerator.
  Value *object = genExpression(ForInStmt->_right);
  Builder.createStoreStackInst(object, baseStorage);

  // The storage for the property name that the enumerator loads:
  auto *propertyStorage =
      Builder.createAllocStackInst(genAnonymousLabelName("prop"));

  /*
    We generate the following loop structure for the for-in loops:

        [ current block ]
        [   get_pname   ]
               |         \
               |          \
               v           \ (on empty object)
    /----> [get_next]       \
    |          |     \       \
    |          |      \       \
    |          |       \       \ ->[ exit block ]
    |          |        \      /
    |    [ body block ]  \____/
    |          |          (on last iteration)
    \__________/
  */

  auto parent = Builder.getInsertionBlock()->getParent();
  auto *exitBlock = Builder.createBasicBlock(parent);
  auto *getNextBlock = Builder.createBasicBlock(parent);
  auto *bodyBlock = Builder.createBasicBlock(parent);

  // Initialize the goto labels.
  auto &label = functionContext->labels[ForInStmt->getLabelIndex()];
  label.breakTarget = exitBlock;
  label.continueTarget = getNextBlock;

  // Create the enumerator:
  Builder.createGetPNamesInst(
      iteratorStorage,
      baseStorage,
      indexStorage,
      sizeStorage,
      exitBlock,
      getNextBlock);

  // Generate the get_next part of the loop:
  Builder.setInsertionBlock(getNextBlock);
  Builder.createGetNextPNameInst(
      propertyStorage,
      baseStorage,
      indexStorage,
      sizeStorage,
      iteratorStorage,
      exitBlock,
      bodyBlock);

  // Emit the loop body and setup the property variable. When done jump into the
  // 'get_next' block and try to do another iteration.
  Builder.setInsertionBlock(bodyBlock);

  // The string property value of the current iteration is saved into this
  // variable.
  auto propertyStringRepr = Builder.createLoadStackInst(propertyStorage);

  // The left hand side of For-In statements can be any lhs expression
  // ("PutValue"). Example:
  //  1. for (x.y in [1,2,3])
  //  2. for (x in [1,2,3])
  //  3. for (var x in [1,2,3])
  // See ES5 $12.6.4 "The for-in Statement"
  LReference lref = createLRef(ForInStmt->_left);
  lref.emitStore(Builder, propertyStringRepr);

  genStatement(ForInStmt->_body);

  Builder.createBranchInst(getNextBlock);

  Builder.setInsertionBlock(exitBlock);
}

void ESTreeIRGen::genReturnStatement(ESTree::ReturnStatementNode *RetStmt) {
  DEBUG(dbgs() << "IRGen Return-stmt.\n");

  Value *Value;
  // Generate IR for the return value, or undefined if this is an empty return
  // statement.
  if (auto *A = RetStmt->_argument) {
    Value = genExpression(A);
  } else {
    Value = Builder.getLiteralUndefined();
  }

  genFinallyBeforeControlChange(RetStmt->surroundingTry, nullptr);
  Builder.createReturnInst(Value);

  // Code that comes after 'return' is dead code. Let's create a new un-linked
  // basic block and keep IRGen in that block. The optimizer will clean things
  // up.
  auto Parent = Builder.getInsertionBlock()->getParent();
  Builder.setInsertionBlock(Builder.createBasicBlock(Parent));
}

/// \returns true if \p node is a constant expression.
static bool isConstantExpr(ESTree::Node *node) {
  switch (node->getKind()) {
    case ESTree::NodeKind::StringLiteral:
    case ESTree::NodeKind::NumericLiteral:
    case ESTree::NodeKind::NullLiteral:
    case ESTree::NodeKind::BooleanLiteral:
      return true;
    default:
      return false;
  }
}

/// \returns true if \p node is the default case.
static inline bool isDefaultCase(ESTree::SwitchCaseNode *caseStmt) {
  // If there is no test field then this is the default block.
  return !caseStmt->_test;
}

bool ESTreeIRGen::areAllCasesConstant(
    ESTree::SwitchStatementNode *switchStmt,
    llvm::SmallVectorImpl<Literal *> &caseLiterals) {
  for (auto &c : switchStmt->_cases) {
    auto *caseStmt = cast<ESTree::SwitchCaseNode>(&c);

    if (isDefaultCase(caseStmt)) {
      caseLiterals.push_back(nullptr);
      continue;
    }

    if (!isConstantExpr(caseStmt->_test))
      return false;

    auto *lit = dyn_cast<Literal>(genExpression(caseStmt->_test));
    assert(lit && "constant expression must compile to a literal");
    caseLiterals.push_back(lit);
  }

  return true;
}

void ESTreeIRGen::genSwitchStatement(ESTree::SwitchStatementNode *switchStmt) {
  DEBUG(dbgs() << "IRGen 'switch' statement.\n");

  {
    llvm::SmallVector<Literal *, 8> caseLiterals{};
    if (areAllCasesConstant(switchStmt, caseLiterals) &&
        caseLiterals.size() > 1) {
      genConstSwitchStmt(switchStmt, caseLiterals);
      return;
    }
  }

  Function *function = Builder.getInsertionBlock()->getParent();
  BasicBlock *exitBlock = Builder.createBasicBlock(function);

  // Unless a default is specified the default case brings us to the exit block.
  BasicBlock *defaultBlock = exitBlock;

  // A BB for each case in the switch statement.
  llvm::SmallVector<BasicBlock *, 8> caseBlocks;

  // Initialize the goto labels.
  auto &label = functionContext->labels[switchStmt->getLabelIndex()];
  label.breakTarget = exitBlock;

  // The discriminator expression.
  Value *discr = genExpression(switchStmt->_discriminant);

  // Sequentially allocate a basic block for each case, compare the discriminant
  // against the case value and conditionally jump to the basic block.
  int caseIndex = -1; // running index of the case's basic block.
  BasicBlock *elseBlock = nullptr; // The next case's condition.

  for (auto &c : switchStmt->_cases) {
    auto *caseStmt = cast<ESTree::SwitchCaseNode>(&c);
    ++caseIndex;
    caseBlocks.push_back(Builder.createBasicBlock(function));

    if (isDefaultCase(caseStmt)) {
      defaultBlock = caseBlocks.back();
      continue;
    }

    auto *caseVal = genExpression(caseStmt->_test);
    auto *pred = Builder.createBinaryOperatorInst(
        caseVal, discr, BinaryOperatorInst::OpKind::StrictlyEqualKind);

    elseBlock = Builder.createBasicBlock(function);
    Builder.createCondBranchInst(pred, caseBlocks[caseIndex], elseBlock);
    Builder.setInsertionBlock(elseBlock);
  }

  Builder.createBranchInst(defaultBlock);

  // Generate the case bodies.
  bool isFirstCase = true;
  caseIndex = -1;
  for (auto &c : switchStmt->_cases) {
    auto *caseStmt = cast<ESTree::SwitchCaseNode>(&c);
    ++caseIndex;

    // Generate the fall-through from the previous block to this one.
    if (!isFirstCase)
      Builder.createBranchInst(caseBlocks[caseIndex]);

    Builder.setInsertionBlock(caseBlocks[caseIndex]);
    genBody(caseStmt->_consequent);
    isFirstCase = false;
  }

  if (!isFirstCase)
    Builder.createBranchInst(exitBlock);

  Builder.setInsertionBlock(exitBlock);
}

void ESTreeIRGen::genConstSwitchStmt(
    ESTree::SwitchStatementNode *switchStmt,
    llvm::SmallVectorImpl<Literal *> &caseLiterals) {
  Function *function = Builder.getInsertionBlock()->getParent();
  BasicBlock *exitBlock = Builder.createBasicBlock(function);

  // Unless a default is specified the default case brings us to the exit block.
  BasicBlock *defaultBlock = exitBlock;

  auto &label = functionContext->labels[switchStmt->getLabelIndex()];
  label.breakTarget = exitBlock;

  // The discriminator expression.
  Value *discr = genExpression(switchStmt->_discriminant);
  // Save the block where we will insert the switch instruction.
  auto *startBlock = Builder.getInsertionBlock();

  // Since this is a constant value switch, duplicates are not allowed and we
  // must filter them. We can conveniently store them in this set.
  llvm::SmallPtrSet<Literal *, 8> valueSet;

  SwitchInst::ValueListType values;
  SwitchInst::BasicBlockListType blocks;

  int caseIndex = -1;
  bool isFirstCase = true;

  for (auto &c : switchStmt->_cases) {
    auto *caseStmt = cast<ESTree::SwitchCaseNode>(&c);
    auto *caseBlock = Builder.createBasicBlock(function);
    ++caseIndex;

    if (isDefaultCase(caseStmt)) {
      defaultBlock = caseBlock;
    } else {
      auto *lit = caseLiterals[caseIndex];

      // Only generate the case and block if this is the first occurence of the
      // value.
      if (valueSet.insert(lit).second) {
        values.push_back(lit);
        blocks.push_back(caseBlock);
      }
    }

    if (!isFirstCase)
      Builder.createBranchInst(caseBlock);

    Builder.setInsertionBlock(caseBlock);
    genBody(caseStmt->_consequent);
    isFirstCase = false;
  }

  if (!isFirstCase)
    Builder.createBranchInst(exitBlock);

  Builder.setInsertionBlock(startBlock);
  Builder.createSwitchInst(discr, defaultBlock, values, blocks);

  Builder.setInsertionBlock(exitBlock);
};

} // namespace irgen
} // namespace hermes
