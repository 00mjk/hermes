/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the LICENSE
 * file in the root directory of this source tree.
 */
#ifndef HERMES_AST_SEMANTICVALIDATOR_H
#define HERMES_AST_SEMANTICVALIDATOR_H

#include "hermes/AST/Context.h"
#include "hermes/AST/ESTree.h"
#include "hermes/Support/SourceErrorManager.h"

#include <deque>

namespace hermes {
namespace sem {

/// Semantic information for a function declaration, expression, method, etc.
class FunctionInfo {
 public:
};

/// Identifier and label tables, populated by the semantic validator. They need
/// to be stored separately from the AST because they have destructors, while
/// the AST is stored in a pool.
class SemContext {
 public:
  /// Create a new instance of \c FunctionInfo.
  FunctionInfo *createFunction() {
    functions_.emplace_back();
    return &functions_.back();
  }

 private:
  std::deque<FunctionInfo> functions_{};
};

/// Perform semantic validation of the entire AST, starting from the specified
/// root, which should be ProgramNode.
bool validateAST(Context &astContext, SemContext &semCtx, ESTree::NodePtr root);

/// Perform semantic validation of an individual function in the given context
/// \param function must be a function node
/// \param strict specifies parent strictness.
bool validateFunctionAST(
    Context &astContext,
    SemContext &semCtx,
    ESTree::NodePtr function,
    bool strict);

} // namespace sem
} // namespace hermes

#endif // HERMES_AST_SEMANTICVALIDATOR_H
