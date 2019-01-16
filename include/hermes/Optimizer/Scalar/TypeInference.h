#ifndef HERMES_OPTIMIZER_SCALAR_TYPEINFERENCE_H
#define HERMES_OPTIMIZER_SCALAR_TYPEINFERENCE_H

#include "hermes/Optimizer/PassManager/Pass.h"

namespace hermes {

/// Infers the types of instructions and enables other optimization passes to
/// optimize based on the deduced types.
class TypeInference : public ModulePass {
  /// Whether to perform CLA on this invocation of Type Inference.
  bool doCLA_;

 public:
  explicit TypeInference(bool doCLA)
      : ModulePass("TypeInference"), doCLA_(doCLA) {}
  ~TypeInference() override = default;

  bool runOnModule(Module *M) override;
};

} // namespace hermes

#endif // HERMES_OPTIMIZER_SCALAR_TYPEINFERENCE_H
