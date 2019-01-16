#include "hermes/Support/SimpleDiagHandler.h"

#include "llvm/ADT/Twine.h"

using llvm::Twine;

namespace hermes {

void SimpleDiagHandler::installInto(llvm::SourceMgr &sourceMgr) {
  sourceMgr.setDiagHandler(handler, this);
}

void SimpleDiagHandler::handler(const llvm::SMDiagnostic &msg, void *ctx) {
  auto *mgr = static_cast<SimpleDiagHandler *>(ctx);
  if (msg.getKind() == llvm::SourceMgr::DK_Error) {
    if (!mgr->hasFirstMessage()) {
      mgr->firstMessage_ = msg;
    }
  }
}

std::string SimpleDiagHandler::getErrorString() const {
  const auto &msg = getFirstMessage();
  return (Twine(msg.getLineNo()) + ":" + Twine(msg.getColumnNo()) + ":" +
          msg.getMessage())
      .str();
}

SimpleDiagHandlerRAII::SimpleDiagHandlerRAII(llvm::SourceMgr &sourceMgr)
    : sourceMgr_(sourceMgr),
      oldHandler_(sourceMgr.getDiagHandler()),
      oldContext_(sourceMgr.getDiagContext()) {
  installInto(sourceMgr);
}

SimpleDiagHandlerRAII::~SimpleDiagHandlerRAII() {
  sourceMgr_.setDiagHandler(oldHandler_, oldContext_);
}

} // namespace hermes
