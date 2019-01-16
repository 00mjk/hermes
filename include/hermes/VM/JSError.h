#ifndef HERMES_VM_JSERROR_H
#define HERMES_VM_JSERROR_H

#include "hermes/VM/JSObject.h"
#include "hermes/VM/NativeArgs.h"
#include "hermes/VM/SmallXString.h"

namespace hermes {
namespace vm {

/// StackTraceInfo holds information of an entry in the stacktrace upon
/// exceptions. We only need to store the CodeBlock and bytecode offset
/// to obtain the full function name/file name/position later when we
/// need to generate the stacktrace string.
struct StackTraceInfo {
  /// The code block of the function.
  CodeBlock *codeBlock;

  /// The bytecode offset where exception was thrown.
  uint32_t bytecodeOffset;

  /// We need to manually manage the reference counting to the runtimemodule
  /// in the constructor and destructor, because CodeBlocks collectively own
  /// the runtime module through the reference counter. We need to make sure
  /// the code block will not be freed if the exeception is passed around
  /// across runtime modules.
  StackTraceInfo(CodeBlock *codeBlock, uint32_t bytecodeOffset)
      : codeBlock(codeBlock), bytecodeOffset(bytecodeOffset) {
    if (codeBlock)
      codeBlock->getRuntimeModule()->addUser();
  }
  ~StackTraceInfo() {
    if (codeBlock)
      codeBlock->getRuntimeModule()->removeUser();
  }

  /// Delegate the copy constructor to the normal constructor, to properly
  /// handle the refcount.
  StackTraceInfo(const StackTraceInfo &that)
      : StackTraceInfo(that.codeBlock, that.bytecodeOffset) {}

  /// Move constructor can steal the RHS's refcount.
  StackTraceInfo(StackTraceInfo &&that)
      : codeBlock(that.codeBlock), bytecodeOffset(that.bytecodeOffset) {
    that.codeBlock = nullptr;
  }

  /// Prevent assignment copy and move.
  void operator=(const StackTraceInfo &that) = delete;
  void operator=(StackTraceInfo &&that) = delete;
};
using StackTrace = std::vector<StackTraceInfo>;
using StackTracePtr = std::unique_ptr<StackTrace>;

/// Error Object.
class JSError final : public JSObject {
 public:
  using Super = JSObject;
  static ObjectVTable vt;
  static bool classof(const GCCell *cell) {
    return cell->getKind() == CellKind::ErrorKind;
  }

  /// Create an Error Object.
  static CallResult<HermesValue> create(
      Runtime *runtime,
      Handle<JSObject> prototype);

  /// If the stack trace is not set, attempt to record it by walking the runtime
  /// stack. If the top call frame indicates a JS callee, but the codeBlock and
  /// ip are not supplied, return without doing anything. This handles the case
  /// when an exception is thrown from within the current code block.
  ///
  /// \param skipTopFrame don't record the topmost frame. This is used when
  ///   we want to skip the Error() constructor itself.
  /// \param codeBlock optional current CodeBlock.
  /// \param ip if \c codeBlock is not \c nullptr, the instruction in the
  ///   current CodeBlock.
  static void recordStackTrace(
      Handle<JSError> selfHandle,
      Runtime *runtime,
      bool skipTopFrame = false,
      CodeBlock *codeBlock = nullptr,
      const Inst *ip = nullptr);

  /// Define the stack setter and getter, for later stack trace creation.
  static ExecutionStatus setupStack(
      Handle<JSError> selfHandle,
      Runtime *runtime);

  /// Set the message property.
  static ExecutionStatus
  setMessage(Handle<JSError> selfHandle, Runtime *runtime, Handle<> message);

  /// \return a pointer to the stack trace, or NULL if the stack trace has been
  /// cleared or not been set.
  const StackTrace *getStackTrace() const {
    return stacktrace_.get();
  }

  /// Upon called, construct the stacktrace string based on
  /// the value of stacktrace_, and reset the stack property to the
  /// stacktrace string.
  static CallResult<HermesValue>
  errorStackGetter(void *, Runtime *runtime, NativeArgs args);

  /// This is called when someone manually set the stack property to
  /// an error object, which should happen rarely. It destroys the
  /// stack access and replace it with a regular property.
  static CallResult<HermesValue>
  errorStackSetter(void *, Runtime *runtime, NativeArgs args);

 protected:
  JSError(
      Runtime *runtime,
      JSObject *proto,
      HiddenClass *clazz,
      JSObjectPropStorage *propStorage)
      : JSObject(runtime, &vt.base, proto, clazz, propStorage) {}

 private:
  static const PropStorage::size_type NEEDED_PROPERTY_SLOTS =
      Super::NEEDED_PROPERTY_SLOTS + 1;

  friend void ErrorBuildMeta(const GCCell *cell, Metadata::Builder &mb);
  static void _finalizeImpl(GCCell *cell, GC *gc);

  /// A pointer to the stack trace, or nullptr if it has not been set.
  StackTracePtr stacktrace_;

  /// If not null, an array of function names as the 'name' property of the
  /// Callables. This is parallel to the stack trace array.
  GCPointer<PropStorage> funcNames_;

  /// Construct the stacktrace string, append to \p stack.
  static void constructStackTraceString(
      Runtime *runtime,
      Handle<JSError> selfHandle,
      SmallU16String<32> &stack);

  /// Append the name of the function at \p index to the given \p str.
  /// \return true on success, false if the name was missing, invalid or empty.
  static bool appendFunctionNameAtIndex(
      Runtime *runtime,
      Handle<JSError> selfHandle,
      size_t index,
      llvm::SmallVectorImpl<char16_t> &str);
};

} // namespace vm
} // namespace hermes
#endif
