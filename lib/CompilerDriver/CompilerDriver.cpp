/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the LICENSE
 * file in the root directory of this source tree.
 */
#include "hermes/CompilerDriver/CompilerDriver.h"

#include "hermes/AST/CommonJS.h"
#include "hermes/AST/Context.h"
#include "hermes/AST/ESTreeJSONDumper.h"
#include "hermes/AST/SemValidate.h"
#include "hermes/BCGen/HBC/BytecodeDisassembler.h"
#include "hermes/BCGen/HBC/HBC.h"
#include "hermes/BCGen/RegAlloc.h"
#include "hermes/ConsoleHost/ConsoleHost.h"
#include "hermes/FlowParser/FlowParser.h"
#include "hermes/IR/Analysis.h"
#include "hermes/IR/IR.h"
#include "hermes/IR/IRBuilder.h"
#include "hermes/IR/IRVerifier.h"
#include "hermes/IR/Instrs.h"
#include "hermes/IRGen/IRGen.h"
#include "hermes/Optimizer/PassManager/PassManager.h"
#include "hermes/Optimizer/PassManager/Pipeline.h"
#include "hermes/Parser/JSONParser.h"
#include "hermes/Parser/JSParser.h"
#include "hermes/Runtime/Libhermes.h"
#include "hermes/SourceMap/SourceMapGenerator.h"
#include "hermes/SourceMap/SourceMapParser.h"
#include "hermes/Support/Algorithms.h"
#include "hermes/Support/MemoryBuffer.h"
#include "hermes/Support/OSCompat.h"
#include "hermes/Support/Warning.h"
#include "hermes/Utils/Dumper.h"
#include "hermes/Utils/Options.h"

#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Process.h"
#include "llvm/Support/SHA1.h"
#include "llvm/Support/raw_ostream.h"

#include "zip/src/zip.h"

#include <sstream>

#define DEBUG_TYPE "hermes"

using llvm::ArrayRef;
using llvm::cast;
using llvm::dyn_cast;
using llvm::Optional;
using llvm::raw_fd_ostream;
using llvm::sys::fs::F_None;
using llvm::sys::fs::F_Text;

using namespace hermes;
using namespace hermes::driver;

namespace cl {
using llvm::cl::desc;
using llvm::cl::Hidden;
using llvm::cl::init;
using llvm::cl::list;
using llvm::cl::opt;
using llvm::cl::Positional;
using llvm::cl::value_desc;
using llvm::cl::values;

/// Encapsulate a compiler flag: for example, "-fflag/-fno-flag", or
/// "-Wflag/-Wno-flag".
class CLFlag {
  std::string yesName_;
  std::string yesHelp_;
  std::string noName_;
  std::string noHelp_;
  llvm::cl::opt<bool> yes_;
  llvm::cl::opt<bool> no_;
  const bool defaultValue_;

 public:
  CLFlag(const CLFlag &) = delete;
  void operator=(CLFlag &) = delete;

  /// \param flagChar is the character that will be prepended to the flag name.
  /// \param name is the name for the command line option
  /// \param defaultValue is the default if neither is specified.
  /// \param desc is the description starting with lower case like " inlining of
  /// functions".
  CLFlag(
      char flagChar,
      const llvm::Twine &name,
      bool defaultValue,
      const llvm::Twine &desc)
      : yesName_((llvm::Twine(flagChar) + name).str()),
        yesHelp_(("Enable " + desc).str()),
        noName_((llvm::Twine(flagChar) + "no-" + name).str()),
        noHelp_(("Disable " + desc).str()),
        yes_(
            StringRef(yesName_),
            llvm::cl::ValueDisallowed,
            llvm::cl::desc(StringRef(yesHelp_))),
        no_(StringRef(noName_),
            llvm::cl::ValueDisallowed,
            llvm::cl::Hidden,
            llvm::cl::desc(StringRef(noHelp_))),
        defaultValue_(defaultValue) {}

  /// Resolve the value of the flag depending on which command line option is
  /// present and which one is last.
  bool getValue() const {
    if (yes_.getPosition() > no_.getPosition())
      return true;
    if (yes_.getPosition() < no_.getPosition())
      return false;
    return defaultValue_;
  }

  /// Casting to bool always makes sense, so no "explicit" needed here.
  operator bool() const {
    return getValue();
  }
};

static list<std::string> InputFilenames(desc("input file"), Positional);

#if !defined(NDEBUG) || defined(LLVM_ENABLE_STATS)
static opt<bool> PrintStats("print-stats", desc("Print statistics"));
#endif

enum class OptLevel {
  O0,
  Og,
  OMax,
};

cl::opt<OptLevel> OptimizationLevel(
    cl::desc("Choose optimization level:"),
    cl::init(OptLevel::Og),
    cl::values(
        clEnumValN(OptLevel::O0, "O0", "No optimizations"),
        clEnumValN(OptLevel::Og, "Og", "Optimizations suitable for debugging"),
        clEnumValN(OptLevel::OMax, "O", "Expensive optimizations")));

static CLFlag StaticBuiltins(
    'f',
    "static-builtins",
    false,
    " recognizing of calls to global functions like Object.keys() statically");

static list<std::string>
    CustomOptimize("custom-opt", desc("Custom optimzations"), Hidden);

static opt<OutputFormatKind> DumpTarget(
    desc("Choose output:"),
    init(None),
    values(
        clEnumValN(None, "no-dump", "Parse only, no output (default)"),
        clEnumValN(DumpAST, "dump-ast", "Dump the AST as text in JSON"),
        clEnumValN(
            DumpTransformedAST,
            "dump-transformed-ast",
            "Dump the transformed AST as text after validation"),
        clEnumValN(DumpIR, "dump-ir", "Dump the IR as text"),
        clEnumValN(DumpLIR, "dump-lir", "Dump the Lowered IR as text"),
        clEnumValN(DumpRA, "dump-ra", "Dump the register-allocated IR as text"),
        clEnumValN(
            DumpLRA,
            "dump-lra",
            "Dump register-allocated Lowered IR as text"),
        clEnumValN(
            DumpPostRA,
            "dump-postra",
            "Dump the Lowered IR after register allocation"),
        clEnumValN(DumpBytecode, "dump-bytecode", "Dump bytecode as text"),
        clEnumValN(EmitBundle, "emit-binary", "Emit compiled binary")));

static opt<bool>
    PrettyJSON("pretty-json", init(false), desc("Pretty print the JSON AST"));

static opt<bool> PrettyDisassemble(
    "pretty-disassemble",
    init(true),
    desc("Pretty print the disassembled bytecode"));

/// Unused option kept for backwards compatibility.
static opt<bool> unused_HermesParser(
    "hermes-parser",
    desc("Treat the input as JavaScript"),
    Hidden);

static opt<bool> FlowParser(
    "Xflow-parser",
    init(false),
    desc("Use libflowparser instead of the hermes parser"),
    Hidden);

static opt<bool> BytecodeMode(
    "b",
    desc("Treat the input as executable bytecode"));

static opt<bool> NonStrictMode("non-strict", desc("Enable non-strict mode."));
static opt<bool> StrictMode("strict", desc("Enable strict mode."));

static opt<bool> LazyCompilation(
    "lazy",
    init(false),
    desc("Compile source lazily when executing (HBC only)"));

/// The following flags are exported so it may be used by the VM driver as well.
opt<bool> BasicBlockProfiling(
    "basic-block-profiling",
    init(false),
    desc("Enable basic block profiling (HBC only)"));

opt<bool>
    EnableEval("enable-eval", init(true), desc("Enable support for eval()"));

static list<std::string> IncludeGlobals(
    "include-globals",
    desc("Include the definitions of global properties (can be "
         "specified more than once)"),
    value_desc("filename"));

enum BytecodeFormatKind {
  HBC,
};

// Enable Debug Options to be specified on the command line
static opt<BytecodeFormatKind> BytecodeFormat(
    "target",
    init(HBC),
    desc("Set the bytecode format:"),
    values(clEnumVal(HBC, "Emit HBC bytecode (default)")));

static opt<std::string> BytecodeOutputFilename("out", desc("Output file name"));

/// Emit debug info for every instruction instead of just the throwing ones.
static opt<bool> EmitDebugInfo(
    "g",
    desc("Emit debug info for all instructions"));

static opt<bool> OutputSourceMap(
    "output-source-map",
    desc("Emit a source map to the output filename with .map extension"));

static opt<bool> DumpOperandRegisters(
    "dump-operand-registers",
    desc("Dump registers assigned to instruction operands"));

static opt<bool> EnableClosureAnalysis(
    "enable-cla",
    desc("Enable closure analysis-based type inference"),
    init(false));

static opt<bool> DumpUseList(
    "dump-instr-uselist",
    desc("Print the use list if the instruction has any users."),
    init(false));

static opt<bool> DumpSourceLocation(
    "dump-source-location",
    desc("Print source location information in IR dumps"));

static opt<bool> DumpBetweenPasses(
    "Xdump-between-passes",
    init(false),
    Hidden,
    desc("Print IR after every optimization pass"));

#ifndef NDEBUG

static opt<bool> LexerOnly(
    "Xlexer-only",
    desc("Only run the lexer on the input (debug builds only)"),
    Hidden);

static opt<bool> ViewCFG("view-cfg", desc("view the CFG."));

#endif

static opt<int> MaxDiagnosticWidth(
    "max-diagnostic-width",
    llvm::cl::desc("Preferred diagnostic maximum width"),
    llvm::cl::init(0));

static opt<bool> EnableCPO(
    "enable-cpo",
    desc("Enable constant property optimizations"),
    init(false));

static opt<bool> EnableUMO(
    "enable-umo",
    desc("Enable uncalled method optimizations"),
    init(false));

static opt<BundlerKind> EnableCrossModuleCLA(
    "enable-xm",
    desc("Enable cross module CLA, if doing CLA"),
    cl::values(
        clEnumValN(BundlerKind::none, "none", "no cross-module optimization"),
        clEnumValN(
            BundlerKind::metromin,
            "metromin",
            "Minified metro bundling")));

static opt<bool>
    CommonJS("commonjs", desc("Use CommonJS modules"), init(false));

static CLFlag StaticRequire(
    'f',
    "static-require",
    false,
    "resolving of CommonJS require() calls at compile time");

static CLFlag Werror('W', "error", false, "Treat all warnings as errors");

static CLFlag UndefinedVariableWarning(
    'W',
    "undefined-variable",
    true,
    "Do not warn when an undefined variable is referenced.");

static opt<bool>
    EnableCallN("enable-calln", desc("Optimize Call to CallN"), init(false));

static opt<bool> ReusePropCache(
    "reuse-prop-cache",
    desc("Reuse property cache entries for same property name"),
    init(true));

static CLFlag Inline('f', "inline", true, "inlining of functions");

static CLFlag
    Outline('f', "outline", false, "IR outlining to reduce code size");

static CLFlag StripFunctionNames(
    'f',
    "strip-function-names",
    false,
    "Strip function names to reduce string table size");

static opt<bool> OutliningPlaceNearCaller(
    "outline-near-caller",
    init(OutliningSettings{}.placeNearCaller),
    desc("Place outlined functions near callers instead of at the end"),
    Hidden);

static opt<unsigned> OutliningMaxRounds(
    "outline-max-rounds",
    init(OutliningSettings{}.maxRounds),
    desc("Maximum number of outlining rounds to perform"),
    Hidden);

static opt<unsigned> OutliningMinLength(
    "outline-min-length",
    init(OutliningSettings{}.minLength),
    desc("Minimum number of instructions to consider outlining"),
    Hidden);

static opt<unsigned> OutliningMinParameters(
    "outline-min-params",
    init(OutliningSettings{}.minParameters),
    desc("Minimum number of parameters in outlined functions"),
    Hidden);

static opt<unsigned> OutliningMaxParameters(
    "outline-max-params",
    init(OutliningSettings{}.maxParameters),
    desc("Maximum number of parameters in outlined functions"),
    Hidden);

static CLFlag DirectEvalWarning(
    'W',
    "direct-eval",
    true,
    "Warning when attempting a direct (local) eval");

static opt<std::string> BaseBytecodeFile(
    "base-bytecode",
    llvm::cl::desc("input base bytecode for delta optimizing mode"),
    llvm::cl::init(""));

static opt<bool> VerifyIR(
    "verify-ir",
#ifdef HERMES_SLOW_DEBUG
    init(true),
#else
    init(false),
    Hidden,
#endif
    desc("Verify the IR after creating it"));

static opt<unsigned> PadFunctionBodiesPercent(
    "pad-function-bodies-percent",
    desc(
        "Add this much garbage after each function body (relative to its size)."),
    init(0),
    Hidden);

} // namespace cl

namespace {

struct FileAndSourceMap {
  /// Input source file. May be a JavaScript source file or an HBC file.
  std::unique_ptr<llvm::MemoryBuffer> file;

  /// SourceMap file. nullptr if not specified by the user.
  std::unique_ptr<llvm::MemoryBuffer> sourceMap;
};

/// Encodes a list of files that are part of a given segment.
using SegmentTableEntry = std::vector<FileAndSourceMap>;

/// Mapping from segment index to the file buffers in that segment.
/// For a given table, table[i][j] is the j-indexed file in segment i.
/// Use an std::map to ensure that the order of iteration is guaranteed here,
/// allowing the assumption that the segments have strictly increasing
/// module IDs. The entry point must be found at table[0][0].
/// If multiple segments or multiple input files are not being used,
/// the only input will be at table[0][0].
using SegmentTable = std::map<uint32_t, SegmentTableEntry>;

/// Read a file at path \p path into a memory buffer. If \p stdinOk is set,
/// allow "-" to mean stdin.
/// \param silent if true, don't print an error message on failure.
/// \return the memory buffer, or nullptr on error, in
/// which case an error message will have been printed to llvm::errs().
std::unique_ptr<llvm::MemoryBuffer> memoryBufferFromFile(
    llvm::StringRef path,
    bool stdinOk = false,
    bool silent = false) {
  auto fileBuf = stdinOk ? llvm::MemoryBuffer::getFileOrSTDIN(path)
                         : llvm::MemoryBuffer::getFile(path);
  if (!fileBuf) {
    if (!silent) {
      llvm::errs() << "Error! Failed to open file: " << path << '\n';
    }
    return nullptr;
  }
  return std::move(*fileBuf);
}

/// Read a file from \p path relative to the root of the zip file \p zip
/// into a memory buffer. Print error messages to llvm::errs().
/// \param zip the zip file to read from (must not be null).
/// \param path the path in the zip file, must be null-terminated.
/// \return the read file, nullptr on error.
std::unique_ptr<llvm::MemoryBuffer>
memoryBufferFromZipFile(zip_t *zip, const char *path, bool silent = false) {
  assert(zip && "zip file must not be null");
  int result = 0;

  result = zip_entry_open(zip, path);
  if (result == -1) {
    if (!silent) {
      llvm::errs() << "Zip error reading " << path << ": File does not exist\n";
    }
    return nullptr;
  }

  size_t size = zip_entry_size(zip);

  // Read data from the file, ensuring null termination of the data.
  std::unique_ptr<llvm::MemoryBuffer> buf =
      llvm::WritableMemoryBuffer::getNewMemBuffer(size, path);
  zip_entry_noallocread(zip, const_cast<char *>(buf->getBufferStart()), size);
  zip_entry_close(zip);

  return buf;
}

/// Open the given file name \p fileName for writing. Print an error to
/// llvm::errs() on failure. Specify \p openFlags as \p F_None or \p F_Text
/// for binary file and text file, respectively.
/// \return the raw stream, or none on failure.
static std::unique_ptr<raw_fd_ostream> openFileForWrite(
    llvm::Twine fileName,
    llvm::sys::fs::OpenFlags openFlags) {
  std::error_code EC;
  llvm::SmallString<32> fileNameOut;
  auto result = llvm::make_unique<raw_fd_ostream>(
      fileName.toStringRef(fileNameOut), EC, openFlags);
  if (EC) {
    llvm::errs() << "Failed to open file " << fileName << ": " << EC.message()
                 << '\n';
    result.reset();
  }
  return result;
}

/// Loads global definitions from MemoryBuffer and adds the definitions to \p
/// declFileList.
/// \return true on success, false on error.
bool loadGlobalDefinition(
    Context &context,
    std::unique_ptr<llvm::MemoryBuffer> content,
    DeclarationFileListTy &declFileList) {
  parser::JSParser jsParser(context, std::move(content));
  auto parsedJs = jsParser.parse();
  if (!parsedJs)
    return false;

  declFileList.push_back(parsedJs.getValue());
  return true;
}

/// Attempt to guess the best error output options by inspecting stderr
SourceErrorOutputOptions guessErrorOutputOptions() {
  SourceErrorOutputOptions result;

  result.showColors = false;
  result.preferredMaxErrorWidth = SourceErrorOutputOptions::UnlimitedWidth;
  if (oscompat::isatty(STDERR_FILENO)) {
    result.showColors = true;
    result.preferredMaxErrorWidth = llvm::sys::Process::StandardErrColumns();
  }

  // Respect MaxDiagnosticWidth if nonzero
  if (cl::MaxDiagnosticWidth < 0) {
    result.preferredMaxErrorWidth = SourceErrorOutputOptions::UnlimitedWidth;
  } else if (cl::MaxDiagnosticWidth > 0) {
    result.preferredMaxErrorWidth = static_cast<size_t>(cl::MaxDiagnosticWidth);
  }
  return result;
}

/// Parse the given files and return a single AST pointer.
/// \return A pointer to the new validated AST, nullptr if parsing failed.
/// If using CJS modules, return a FunctionExpressionNode, else a FileNode.
ESTree::NodePtr parseJS(
    std::shared_ptr<Context> &context,
    sem::SemContext &semCtx,
    std::unique_ptr<llvm::MemoryBuffer> fileBuf,
    bool wrapCJSModule = false) {
  assert(fileBuf && "Need a file to compile");
  assert(context && "Need a context to compile using");

  int fileBufId =
      context->getSourceErrorManager().addNewSourceBuffer(std::move(fileBuf));
  auto mode = parser::FullParse;

  if (context->isLazyCompilation()) {
    if (!parser::JSParser::preParseBuffer(*context, fileBufId)) {
      return nullptr;
    }
    mode = parser::LazyParse;
  }

  Optional<ESTree::FileNode *> parsedJs;

#ifdef HERMES_USE_FLOWPARSER
  if (cl::FlowParser) {
    parsedJs = parser::parseFlowParser(*context, fileBufId);
  } else
#endif
  {
    parser::JSParser jsParser(*context, fileBufId, mode);
    parsedJs = jsParser.parse();
  }
  if (!parsedJs)
    return nullptr;
  ESTree::NodePtr parsedAST = parsedJs.getValue();

  if (wrapCJSModule) {
    parsedAST =
        hermes::wrapCJSModule(context, cast<ESTree::FileNode>(parsedAST));
    if (!parsedAST) {
      return nullptr;
    }
  }

  if (cl::DumpTarget == DumpAST) {
    hermes::dumpESTreeJSON(
        llvm::outs(), parsedAST, cl::PrettyJSON /* pretty */);
  }

  if (!hermes::sem::validateAST(*context, semCtx, parsedAST)) {
    return nullptr;
  }

  if (cl::DumpTarget == DumpTransformedAST) {
    hermes::dumpESTreeJSON(
        llvm::outs(), parsedAST, cl::PrettyJSON /* pretty */);
  }

  return parsedAST;
}

/// Apply custom logic for flag initialization.
void setFlagDefaults() {
  // We haven't been given any file names; just use "-", which acts as stdin.
  if (cl::InputFilenames.empty()) {
    cl::InputFilenames.push_back("-");
  }

  // If bytecode mode is not explicitly specified, check the input extension.
  // of the input file.
  if (!cl::BytecodeMode && cl::InputFilenames.size() == 1 &&
      llvm::sys::path::extension(cl::InputFilenames[0]) == ".hbc") {
    cl::BytecodeMode = true;
  }
}

/// Validate command line flags.
/// \return true if the flags are valid, false if not. On a false return, an
/// error will have been printed to stderr.
bool validateFlags() {
  // Helper to print an error message and return false.
  bool errored = false;
  auto err = [&errored](const char *msg) {
    if (!errored) {
      llvm::errs() << msg << '\n';
      errored = true;
    }
  };

  // Validate strict vs non strict mode.
  if (cl::NonStrictMode && cl::StrictMode) {
    err("Error! Cannot use both -strict and -non-strict");
  }

  // Validate bytecode output file.
  if (cl::DumpTarget == EmitBundle && cl::BytecodeOutputFilename.empty() &&
      oscompat::isatty(STDOUT_FILENO)) {
    // To skip this check and trash the terminal, use -out /dev/stdout.
    err("Refusing to write binary bundle to terminal.\n"
        "Specify output file with -out filename.");
  }

  // Validate lazy compilation flags.
  if (cl::LazyCompilation) {
    if (cl::BytecodeFormat != cl::BytecodeFormatKind::HBC)
      err("-lazy only works with -target=HBC");
    if (cl::OptimizationLevel > cl::OptLevel::Og)
      err("-lazy does not work with -O");
    if (cl::BytecodeMode) {
      err("-lazy doesn't make sense with bytecode");
    }
    if (!cl::CustomOptimize.empty()) {
      // We don't currently pass these around to be applied later.
      err("-lazy doesn't allow custom optimizations");
    }
    if (cl::CommonJS) {
      err("-lazy doesn't support CommonJS modules");
    }
  }

  // Validate flags for more than one input file.
  if (cl::InputFilenames.size() > 1) {
    if (cl::BytecodeMode)
      err("Hermes can only load one bytecode file.");
    if (cl::BytecodeFormat != cl::BytecodeFormatKind::HBC)
      err("Multiple files are only supported with HBC.");
    if (!cl::CommonJS)
      err("Multiple files must use CommonJS modules.");
  }

  // Validate source map output flags.
  if (cl::OutputSourceMap) {
    if (cl::BytecodeOutputFilename.empty())
      err("-output-source-map requires -out to be set");
    if (cl::BytecodeFormat != cl::BytecodeFormatKind::HBC)
      err("-output-source-map requires HBC target");
    if (cl::DumpTarget != EmitBundle)
      err("-output-source-map only works with -emit-binary");
  }

  // Validate bytecode dumping flags.
  if (cl::BytecodeMode && cl::DumpTarget != None) {
    if (cl::BytecodeFormat != cl::BytecodeFormatKind::HBC)
      err("Only Hermes bytecode files may be dumped");
    if (cl::DumpTarget != DumpBytecode)
      err("You can only dump bytecode for HBC bytecode file.");
  }

  return !errored;
}

/// Create a Context, respecting the command line flags.
/// \return the Context.
std::shared_ptr<Context> createContext(
    std::unique_ptr<Context::ResolutionTable> resolutionTable,
    std::vector<Context::SegmentRange> segmentRanges) {
  TypeCheckerSettings typeCheckerOpts;
  typeCheckerOpts.closureAnalysis = cl::EnableClosureAnalysis;

  CodeGenerationSettings codeGenOpts;
  codeGenOpts.dumpOperandRegisters = cl::DumpOperandRegisters;
  codeGenOpts.dumpUseList = cl::DumpUseList;
  codeGenOpts.dumpSourceLocation = cl::DumpSourceLocation;
  codeGenOpts.dumpIRBetweenPasses = cl::DumpBetweenPasses;
  if (cl::BytecodeFormat == cl::BytecodeFormatKind::HBC) {
    codeGenOpts.unlimitedRegisters = false;
  }

  OptimizationSettings optimizationOpts;
  optimizationOpts.constantPropertyOptimizations = cl::EnableCPO;
  optimizationOpts.uncalledMethodOptimizations = cl::EnableUMO;
  optimizationOpts.crossModuleClosureAnalysis =
      cl::EnableCrossModuleCLA.getValue();

  // Enable aggressiveNonStrictModeOptimizations if the target is HBC.
  optimizationOpts.aggressiveNonStrictModeOptimizations =
      cl::BytecodeFormat == cl::BytecodeFormatKind::HBC;

  optimizationOpts.inlining = cl::OptimizationLevel != cl::OptLevel::O0 &&
      cl::BytecodeFormat == cl::BytecodeFormatKind::HBC && cl::Inline;
  optimizationOpts.outlining =
      cl::OptimizationLevel != cl::OptLevel::O0 && cl::Outline;

  optimizationOpts.outliningSettings.placeNearCaller =
      cl::OutliningPlaceNearCaller;
  optimizationOpts.outliningSettings.maxRounds = cl::OutliningMaxRounds;
  optimizationOpts.outliningSettings.minLength = cl::OutliningMinLength;
  optimizationOpts.outliningSettings.minParameters = cl::OutliningMinParameters;
  optimizationOpts.outliningSettings.maxParameters = cl::OutliningMaxParameters;

  optimizationOpts.callN = cl::EnableCallN;
  optimizationOpts.reusePropCache = cl::ReusePropCache;

  optimizationOpts.staticBuiltins = cl::StaticBuiltins;
  optimizationOpts.staticRequire = cl::StaticRequire;

  auto context = std::make_shared<Context>(
      codeGenOpts,
      typeCheckerOpts,
      optimizationOpts,
      std::move(resolutionTable),
      std::move(segmentRanges));

  // Default is non-strict mode.
  context->setStrictMode(!cl::NonStrictMode && cl::StrictMode);
  context->setEnableEval(cl::EnableEval);
  context->getSourceErrorManager().setOutputOptions(guessErrorOutputOptions());
  context->getSourceErrorManager().setWarningsAreErrors(cl::Werror);
  context->getSourceErrorManager().setWarningStatus(
      Warning::UndefinedVariable, cl::UndefinedVariableWarning);
  context->getSourceErrorManager().setWarningStatus(
      Warning::DirectEval, cl::DirectEvalWarning);

  if (cl::CommonJS) {
    context->setUseCJSModules(true);
  }

  if (cl::EmitDebugInfo) {
    context->setDebugInfoSetting(DebugInfoSetting::ALL);
  } else {
    context->setDebugInfoSetting(DebugInfoSetting::THROWING);
  }
  return context;
}

/// Parse \p file into a JSON value.
/// \param alloc the allocator to use for JSON parsing.
/// \return a metadata JSONObject allocated in the user-specified allocator,
/// nullptr on failure. All error messages are printed to stderr.
::hermes::parser::JSONValue *parseJSONFile(
    std::unique_ptr<llvm::MemoryBuffer> &file,
    ::hermes::parser::JSLexer::Allocator &alloc) {
  using namespace ::hermes::parser;
  JSONFactory factory(alloc);
  SourceErrorManager sm;
  JSONParser parser(factory, *file, sm);
  auto root = parser.parse();
  if (!root) {
    llvm::errs()
        << "Failed to parse metadata: Unable to parse a valid JSON object\n";
    return nullptr;
  }
  return root.getValue();
}

/// Read input filenames from the given path and populate the files in \p
/// fileBufs.
/// In case of failure, ensure fileBufs is empty.
/// \param inputPath the path to the directory or zip file containing metadata
/// and files.
/// \param fileBufs[out] table of file buffers.
/// \param alloc the allocator to use for JSON parsing of metadata.
/// \return a pointer to the metadata JSON object, nullptr on failure.
::hermes::parser::JSONObject *readInputFilenamesFromDirectoryOrZip(
    llvm::StringRef inputPath,
    SegmentTable &fileBufs,
    std::vector<Context::SegmentRange> &segmentRanges,
    ::hermes::parser::JSLexer::Allocator &alloc,
    struct zip_t *zip) {
  // Get the path to the actual file given the path relative to the folder root.
  auto getFullPath =
      [&inputPath](llvm::StringRef relPath) -> llvm::SmallString<32> {
    llvm::SmallString<32> path{};
    llvm::sys::path::append(
        path, llvm::sys::path::Style::posix, inputPath, relPath);
    llvm::sys::path::remove_dots(path, false, llvm::sys::path::Style::posix);
    return path;
  };
  // Get the path to the actual file given the path relative to the folder root.
  // Ensures null termination.
  auto getZipPath = [](llvm::StringRef relPath) -> llvm::SmallString<32> {
    llvm::SmallString<32> path{relPath};
    llvm::sys::path::remove_dots(path, false, llvm::sys::path::Style::posix);
    return path;
  };

  auto getFile = [&zip, &getZipPath, &getFullPath](
                     llvm::Twine path, bool silent = false) {
    llvm::SmallString<32> out{};
    return zip ? memoryBufferFromZipFile(
                     zip, getZipPath(path.toStringRef(out)).c_str(), silent)
               : memoryBufferFromFile(
                     getFullPath(path.toStringRef(out)), false, silent);
  };

  auto metadataBuf = getFile("metadata.json");
  if (!metadataBuf) {
    llvm::errs()
        << "Failed to read metadata: Input must contain a metadata.json file\n";
    return nullptr;
  }

  auto *metadataVal = parseJSONFile(metadataBuf, alloc);
  if (!metadataVal) {
    // parseJSONFile prints any error messages.
    return nullptr;
  }

  // Pull data from the metadata JSON object into C++ data structures.
  // The metadata format is documented at doc/Modules.md.

  auto *metadata = dyn_cast<parser::JSONObject>(metadataVal);
  if (!metadata) {
    llvm::errs() << "Metadata must be a JSON object\n";
    return nullptr;
  }

  auto *segments =
      llvm::dyn_cast_or_null<parser::JSONObject>(metadata->get("segments"));
  if (!segments) {
    llvm::errs() << "Metadata must contain segment information\n";
    return nullptr;
  }

  uint32_t moduleIdx = 0;

  for (auto it : *segments) {
    Context::SegmentRange range;
    if (it.first->str().getAsInteger(10, range.segment)) {
      // getAsInteger returns true to signal error.
      llvm::errs()
          << "Metadata segment indices must be unsigned integers: Found "
          << it.first->str() << '\n';
      return nullptr;
    }

    auto *segment = llvm::dyn_cast_or_null<parser::JSONArray>(it.second);
    if (!segment) {
      llvm::errs() << "Metadata segment information must be an array\n";
      return nullptr;
    }

    range.first = moduleIdx;
    range.last = moduleIdx + segment->size() - 1;
    moduleIdx += segment->size();

    SegmentTableEntry segmentBufs{};
    for (auto val : *segment) {
      auto *relPath = llvm::dyn_cast_or_null<parser::JSONString>(val);
      if (!relPath) {
        llvm::errs() << "Segment paths must be strings\n";
        return nullptr;
      }
      auto fileBuf = getFile(relPath->str());
      if (!fileBuf) {
        return nullptr;
      }
      auto mapBuf = getFile(llvm::Twine(relPath->str(), ".map"), true);
      // mapBuf is optional, so simply pass it through if it's null.
      segmentBufs.push_back({std::move(fileBuf), std::move(mapBuf)});
    }
    auto emplaceRes = fileBufs.emplace(range.segment, std::move(segmentBufs));
    if (!emplaceRes.second) {
      llvm::errs() << "Duplicate segment entry in metadata: " << range.segment
                   << "\n";
      return nullptr;
    }

    segmentRanges.push_back(std::move(range));
  }

  return metadata;
}

/// Read a resolution table. Given a file name, it maps every require string
/// to the actual file which must be required.
/// Prints out error messages to stderr in case of failure.
/// \param metadata the full metadata JSONObject. Contains "resolutionTable".
/// \return the resolution table read from the metadata, nullptr on failure.
std::unique_ptr<Context::ResolutionTable> readResolutionTable(
    ::hermes::parser::JSONObject *metadata) {
  assert(metadata && "No metadata to read resolution table from");

  using namespace ::hermes::parser;

  auto result = hermes::make_unique<Context::ResolutionTable>();

  JSONObject *resolutionTable =
      llvm::dyn_cast_or_null<JSONObject>(metadata->get("resolutionTable"));
  if (!resolutionTable) {
    return nullptr;
  }

  for (auto itFile : *resolutionTable) {
    llvm::StringRef filename =
        llvm::sys::path::remove_leading_dotslash(itFile.first->str());
    JSONObject *fileTable = llvm::dyn_cast<JSONObject>(itFile.second);
    if (!fileTable) {
      llvm::errs() << "Invalid value in resolution table for file: " << filename
                   << '\n';
      return nullptr;
    }
    Context::ResolutionTableEntry map{};
    for (auto itEntry : *fileTable) {
      JSONString *src = itEntry.first;
      JSONString *dstJSON = llvm::dyn_cast<JSONString>(itEntry.second);
      if (!dstJSON) {
        llvm::errs() << "Invalid value in resolution table: " << filename << '@'
                     << src->str() << '\n';
        return nullptr;
      }
      llvm::StringRef dst =
          llvm::sys::path::remove_leading_dotslash(dstJSON->str());
      auto emplaceRes = map.try_emplace(src->str(), dst);
      if (!emplaceRes.second) {
        llvm::errs() << "Duplicate entry in resolution table: " << filename
                     << '@' << src->str() << '\n';
        return nullptr;
      }
    }
    auto emplaceRes = result->try_emplace(filename, std::move(map));
    if (!emplaceRes.second) {
      llvm::errs() << "Duplicate entry in resolution table for file: "
                   << filename << '\n';
      return nullptr;
    }
  }

  return result;
}

/// \return a SourceMapGenerator, whose sources respect the command line flags.
SourceMapGenerator createSourceMapGenerator(std::shared_ptr<Context> context) {
  SourceMapGenerator result;
  std::vector<std::string> sources{cl::InputFilenames};
  if (context->getUseCJSModules())
    sources.insert(sources.begin(), "<global>");
  result.setSources(std::move(sources));
  return result;
}

/// Generate IR for CJS modules into the Module \p M for the source files in
/// \p fileBufs. Treat the first element in fileBufs as the entry point.
/// \param inputSourceMaps the parsed versions of the input source maps,
/// in the order in which the files were compiled.
/// \return true on success, false on error, in which case an error will be
/// printed.
bool generateIRForSourcesAsCJSModules(
    Module &M,
    sem::SemContext &semCtx,
    const DeclarationFileListTy &declFileList,
    SegmentTable fileBufs,
    SourceMapGenerator *sourceMapGen) {
  auto context = M.shareContext();
  llvm::SmallString<64> rootPath{fileBufs[0][0].file->getBufferIdentifier()};
  llvm::sys::path::remove_filename(rootPath, llvm::sys::path::Style::posix);

  // Construct a MemoryBuffer for our global entry point.
  llvm::SmallString<64> entryPointFilename{
      fileBufs[0][0].file->getBufferIdentifier()};
  llvm::sys::path::replace_path_prefix(
      entryPointFilename, rootPath, "./", llvm::sys::path::Style::posix);

  // The top-level function is empty, due to the fact that it is not intended to
  // be executed. The Runtime must choose and execute the correct entry point
  // (main) module, from which other modules may be `require`d.
  auto globalMemBuffer = llvm::MemoryBuffer::getMemBufferCopy("", "<global>");

  auto *globalAST = parseJS(context, semCtx, std::move(globalMemBuffer));
  generateIRFromESTree(globalAST, &M, declFileList, {});

  SourceMapParser sourceMapParser{};
  std::vector<std::unique_ptr<SourceMap>> inputSourceMaps{};

  Function *topLevelFunction = M.getTopLevelFunction();
  for (auto &entry : fileBufs) {
    for (auto &fileBufAndMap : entry.second) {
      auto &fileBuf = fileBufAndMap.file;
      llvm::SmallString<64> filename{fileBuf->getBufferIdentifier()};
      llvm::sys::path::replace_path_prefix(
          filename, rootPath, "./", llvm::sys::path::Style::posix);
      auto *ast = parseJS(context, semCtx, std::move(fileBuf), true);
      if (!ast) {
        return false;
      }
      generateIRForCJSModule(
          cast<ESTree::FunctionExpressionNode>(ast),
          llvm::sys::path::remove_leading_dotslash(filename),
          &M,
          topLevelFunction,
          declFileList);
      if (fileBufAndMap.sourceMap) {
        auto inputMap =
            sourceMapParser.parse(fileBufAndMap.sourceMap->getBuffer());
        if (!inputMap) {
          // parse() returns nullptr on failure.
          llvm::errs() << "Error: Invalid source map: "
                       << fileBufAndMap.sourceMap->getBufferIdentifier()
                       << '\n';
          return false;
        }
        inputSourceMaps.push_back(std::move(inputMap));
      } else {
        inputSourceMaps.push_back(nullptr);
      }
    }
  }

  if (sourceMapGen) {
    sourceMapGen->setInputSourceMaps(std::move(inputSourceMaps));
  }

  return true;
}

/// Disassemble the BCProvider \p bytecode to the output stream specified by the
/// command line flags. \return a CompileResult for the disassembly.
CompileResult disassembleBytecode(std::unique_ptr<hbc::BCProvider> bytecode) {
  assert(
      cl::BytecodeFormat == cl::BytecodeFormatKind::HBC &&
      "validateFlags() should enforce only HBC files may be disassembled");

  std::unique_ptr<raw_fd_ostream> fileOS;
  if (!cl::BytecodeOutputFilename.empty()) {
    fileOS = openFileForWrite(cl::BytecodeOutputFilename, F_Text);
    if (!fileOS)
      return InputFileError;
  }

  hbc::DisassemblyOptions disassemblyOptions = cl::PrettyDisassemble
      ? hbc::DisassemblyOptions::Pretty
      : hbc::DisassemblyOptions::None;
  auto &OS = fileOS ? *fileOS : llvm::outs();
  hbc::BytecodeDisassembler disassembler(std::move(bytecode));
  disassembler.setOptions(disassemblyOptions);
  disassembler.disassemble(OS);
  return Success;
}

/// Load the base bytecode provider as requested by command line options.
/// \return the base bytecode provider.
std::unique_ptr<hbc::BCProviderFromBuffer> loadBaseBytecodeProvider() {
  assert(!cl::BaseBytecodeFile.empty() && "No base bytecode file requested");
  auto fileBuf = memoryBufferFromFile(cl::BaseBytecodeFile);
  if (!fileBuf)
    return nullptr;
  // Transfer ownership to an owned memory buffer.
  auto ownedBuf = llvm::make_unique<OwnedMemoryBuffer>(std::move(fileBuf));
  auto ret = hbc::BCProviderFromBuffer::createBCProviderFromBuffer(
      std::move(ownedBuf));
  if (!ret.first) {
    llvm::errs() << "Error deserializing base bytecode: " << ret.second;
    return nullptr;
  }
  return std::move(ret.first);
}

/// Process the bytecode file given in \p fileBuf. Disassemble it if requested,
/// otherwise return it as the CompileResult artifact. \return a compile result.
CompileResult processBytecodeFile(std::unique_ptr<llvm::MemoryBuffer> fileBuf) {
  assert(cl::BytecodeMode && "Input files must be bytecode");
  assert(
      cl::BytecodeFormat == cl::BytecodeFormatKind::HBC &&
      "Only HBC bytecode format may be loaded");

  bool isMmapped =
      fileBuf->getBufferKind() == llvm::MemoryBuffer::MemoryBuffer_MMap;
  char *bufStart = const_cast<char *>(fileBuf->getBufferStart());
  size_t bufSize = fileBuf->getBufferSize();
  std::string filename = fileBuf->getBufferIdentifier();

  std::unique_ptr<hbc::BCProviderFromBuffer> bytecode;
  auto buffer = llvm::make_unique<OwnedMemoryBuffer>(std::move(fileBuf));
  auto ret =
      hbc::BCProviderFromBuffer::createBCProviderFromBuffer(std::move(buffer));
  if (!ret.first) {
    llvm::errs() << "Error deserializing bytecode: " << ret.second;
    return InputFileError;
  }
  bytecode = std::move(ret.first);
  if (cl::DumpTarget != None) {
    assert(
        cl::DumpTarget == DumpBytecode &&
        "validateFlags() should enforce bytecode files "
        "may only have a dump target of bytecode");
    return disassembleBytecode(std::move(bytecode));
  } else {
    CompileResult result{Success};
    result.bytecodeProvider = std::move(bytecode);
    result.bytecodeBufferInfo =
        BytecodeBufferInfo{isMmapped, bufStart, bufSize, std::move(filename)};
    return result;
  }
}

/// Compile the given module \p M with the options \p genOptions in a form
/// suitable for immediate execution (i.e. no expectation of persistence).
/// \return the compile result.
CompileResult generateBytecodeForExecution(
    Module &M,
    const BytecodeGenerationOptions &genOptions) {
  std::shared_ptr<Context> context = M.shareContext();
  CompileResult result{Success};
  if (cl::BytecodeFormat == cl::BytecodeFormatKind::HBC) {
    // Lazy compilation requires that the context stay alive.
    if (context->isLazyCompilation())
      result.context = context;
    result.bytecodeProvider = hbc::BCProviderFromSrc::createBCProviderFromSrc(
        hbc::generateBytecodeModule(&M, M.getTopLevelFunction(), genOptions));

  } else {
    llvm_unreachable("Invalid bytecode kind for execution");
    result = InvalidFlags;
  }
  return result;
}

/// Compile the module \p M with the options \p genOptions, serializing the
/// result to \p OS. If sourceMapGenOrNull is not null, populate it.
/// \return the CompileResult.
CompileResult generateBytecodeForSerialization(
    raw_ostream &OS,
    Module &M,
    const BytecodeGenerationOptions &genOptions,
    const SHA1 &sourceHash,
    OptValue<Context::SegmentRange> range,
    SourceMapGenerator *sourceMapGenOrNull) {
  // Serialize the bytecode to the file.
  if (cl::BytecodeFormat == cl::BytecodeFormatKind::HBC) {
    std::unique_ptr<hbc::BCProviderFromBuffer> baseBCProvider;
    // Load the base bytecode if requested.
    if (!cl::BaseBytecodeFile.empty()) {
      baseBCProvider = loadBaseBytecodeProvider();
      if (!baseBCProvider)
        return InputFileError;
    }
    auto bytecodeModule = hbc::generateBytecode(
        &M,
        OS,
        genOptions,
        sourceHash,
        range,
        sourceMapGenOrNull,
        std::move(baseBCProvider));

    if (cl::DumpTarget == DumpBytecode) {
      disassembleBytecode(hbc::BCProviderFromSrc::createBCProviderFromSrc(
          std::move(bytecodeModule)));
    }
  } else {
    llvm_unreachable("Invalid bytecode kind");
  }
  return Success;
}

/// Compiles the given files \p fileBufs with the context \p context,
/// respecting the command line flags.
/// \return a CompileResult containing the compilation status and artifacts.
CompileResult processSourceFiles(
    std::shared_ptr<Context> context,
    SegmentTable fileBufs) {
  assert(!fileBufs.empty() && "Need at least one file to compile");
  assert(context && "Need a context to compile using");
  assert(!cl::BytecodeMode && "Input files must not be bytecode");

  llvm::SHA1 hasher;
  for (const auto &entry : fileBufs) {
    for (const auto &fileAndMap : entry.second) {
      const auto &file = fileAndMap.file;
      hasher.update(
          llvm::StringRef(file->getBufferStart(), file->getBufferSize()));
    }
  }
  auto rawFinalHash = hasher.final();
  SHA1 sourceHash{};
  assert(
      rawFinalHash.size() == SHA1_NUM_BYTES && "Incorrect length of SHA1 hash");
  std::copy(rawFinalHash.begin(), rawFinalHash.end(), sourceHash.begin());
#ifndef NDEBUG
  if (cl::LexerOnly) {
    unsigned count = 0;
    for (auto &entry : fileBufs) {
      for (auto &fileAndMap : entry.second) {
        parser::JSLexer jsLexer(
            std::move(fileAndMap.file),
            context->getSourceErrorManager(),
            context->getAllocator());
        while (jsLexer.advance()->getKind() != parser::TokenKind::eof)
          ++count;
      }
    }
    llvm::outs() << count << " tokens lexed\n";
    return Success;
  }
#endif

  // A list of parsed global definition files.
  DeclarationFileListTy declFileList;

  // Load the runtime library.
  std::unique_ptr<llvm::MemoryBuffer> libBuffer;
  switch (cl::BytecodeFormat) {
    case cl::BytecodeFormatKind::HBC:
      libBuffer = llvm::MemoryBuffer::getMemBuffer(libhermes);
      break;
  }
  if (!loadGlobalDefinition(*context, std::move(libBuffer), declFileList)) {
    return LoadGlobalsFailed;
  }

  // Load the global property definitions.
  for (const auto &fileName : cl::IncludeGlobals) {
    auto fileBuf = memoryBufferFromFile(fileName);
    if (!fileBuf)
      return InputFileError;
    DEBUG(
        llvm::dbgs() << "Parsing global definitions from " << fileName << '\n');
    if (!loadGlobalDefinition(*context, std::move(fileBuf), declFileList)) {
      return LoadGlobalsFailed;
    }
  }

  // Enable lazy compilation if requested.
  context->setLazyCompilation(cl::LazyCompilation);

  // Create the source map if requested.
  llvm::Optional<SourceMapGenerator> sourceMap;
  if (cl::OutputSourceMap) {
    sourceMap = createSourceMapGenerator(context);
  }

  Module M(context);
  sem::SemContext semCtx{};

  if (context->getUseCJSModules()) {
    // Allow the IR generation function to populate inputSourceMaps to ensure
    // proper source map ordering.
    if (!generateIRForSourcesAsCJSModules(
            M,
            semCtx,
            declFileList,
            std::move(fileBufs),
            sourceMap ? &*sourceMap : nullptr)) {
      return ParsingFailed;
    }
  } else {
    ESTree::NodePtr ast =
        parseJS(context, semCtx, std::move(fileBufs[0][0].file));
    if (!ast) {
      return ParsingFailed;
    }
    generateIRFromESTree(ast, &M, declFileList, {});
  }

  // Bail out if there were any errors. We can't ensure that the module is in
  // a valid state.
  if (auto N = context->getSourceErrorManager().getErrorCount()) {
    llvm::errs() << "Emitted " << N << " errors. exiting.\n";
    return ParsingFailed;
  }

  // Run custom optimization pipeline.
  if (!cl::CustomOptimize.empty()) {
    std::vector<std::string> opts(
        cl::CustomOptimize.begin(), cl::CustomOptimize.end());
    if (!runCustomOptimizationPasses(M, opts)) {
      llvm::errs() << "Invalid custom optimizations selected.\n\n"
                   << PassManager::getCustomPassText();
      return InvalidFlags;
    }
  } else {
    switch (cl::OptimizationLevel) {
      case cl::OptLevel::O0:
        runNoOptimizationPasses(M);
        break;
      case cl::OptLevel::Og:
        runDebugOptimizationPasses(M);
        break;
      case cl::OptLevel::OMax:
        runFullOptimizationPasses(M);
        break;
    }
  }

  if (cl::DumpTarget == DumpIR) {
    M.dump();
  }

#ifndef NDEBUG
  if (cl::ViewCFG) {
    M.viewGraph();
  }
#endif

  // In dbg builds, verify the module before we emit bytecode.
  if (cl::VerifyIR) {
    bool failedVerification = verifyModule(M, &llvm::errs());
    if (failedVerification) {
      M.dump();
    }
    assert(!failedVerification && "Module verification failed!");
  }

  BytecodeGenerationOptions genOptions{cl::DumpTarget};
  genOptions.optimizationEnabled = cl::OptimizationLevel > cl::OptLevel::Og;
  genOptions.prettyDisassemble = cl::PrettyDisassemble;
  genOptions.basicBlockProfiling = cl::BasicBlockProfiling;
  genOptions.staticBuiltinsEnabled = cl::StaticBuiltins;
  genOptions.padFunctionBodiesPercent = cl::PadFunctionBodiesPercent;

  // If the user requests to output a source map, then do not also emit debug
  // info into the bytecode.
  genOptions.stripDebugInfoSection = cl::OutputSourceMap;

  genOptions.stripFunctionNames = cl::StripFunctionNames;

  // If the dump target is None, return bytecode in an executable form.
  if (cl::DumpTarget == None) {
    assert(
        !sourceMap &&
        "validateFlags() should enforce no source map output for execution");
    return generateBytecodeForExecution(M, genOptions);
  }

  CompileResult result{Success};
  if (context->getSegmentRanges().size() < 2) {
    // Ok, we're going to return the bytecode in a serializable form.
    // Open the output file, if any.
    std::unique_ptr<raw_fd_ostream> fileOS{};
    if (!cl::BytecodeOutputFilename.empty()) {
      fileOS = openFileForWrite(cl::BytecodeOutputFilename, F_None);
      if (!fileOS)
        return OutputFileError;
    }
    auto &OS = fileOS ? *fileOS : llvm::outs();
    auto result = generateBytecodeForSerialization(
        OS,
        M,
        genOptions,
        sourceHash,
        llvm::None,
        sourceMap ? sourceMap.getPointer() : nullptr);
    if (result.status != Success) {
      return result;
    }
  } else {
    StringRef base = cl::BytecodeOutputFilename;
    for (const auto &range : context->getSegmentRanges()) {
      auto fileOS = openFileForWrite(
          range.segment == 0 ? base
                             : (Twine(base) + "." + Twine(range.segment)),
          F_None);
      if (!fileOS)
        return OutputFileError;
      auto &OS = *fileOS;
      auto segResult = generateBytecodeForSerialization(
          OS,
          M,
          genOptions,
          sourceHash,
          range,
          sourceMap ? sourceMap.getPointer() : nullptr);
      if (segResult.status != Success) {
        return segResult;
      }
    }
    result = Success;
  }

  // Output the source map if requested.
  if (cl::OutputSourceMap) {
    std::string mapFilePath = cl::BytecodeOutputFilename + ".map";
    auto OS = openFileForWrite(mapFilePath, F_Text);
    if (!OS)
      return OutputFileError;
    sourceMap->outputAsJSON(*OS);
  }

  return result;
}

/// Print the Hermes version to the stream \p s, outputting the \p vmStr (which
/// may be empty).
void printHermesVersion(llvm::raw_ostream &s, const char *vmStr = "") {
  s << "Hermes JavaScript compiler" << vmStr << ".\n"
    << "  HBC bytecode version: " << hermes::hbc::BYTECODE_VERSION << "\n"
    << "\n"
    << "  Features:\n"
#ifdef HERMES_ENABLE_DEBUGGER
    << "    Debugger\n"
#endif
    << "    Zip file input\n";
}

} // namespace

namespace hermes {
namespace driver {

void printHermesCompilerVMVersion(llvm::raw_ostream &s) {
  printHermesVersion(s, " and Virtual Machine");
}
void printHermesCompilerVersion(llvm::raw_ostream &s) {
  printHermesVersion(s);
}

CompileResult compileFromCommandLineOptions() {
#if !defined(NDEBUG) || defined(LLVM_ENABLE_STATS)
  if (cl::PrintStats)
    hermes::EnableStatistics();
#endif

  // Set up and validate flags.
  setFlagDefaults();
  if (!validateFlags())
    return InvalidFlags;

  // Load input files.
  SegmentTable fileBufs{};

  // Allocator for the metadata table.
  ::hermes::parser::JSLexer::Allocator metadataAlloc;

  // Resolution table in metadata, null if none could be read.
  std::unique_ptr<Context::ResolutionTable> resolutionTable = nullptr;

  // Segment table in metadata.
  std::vector<Context::SegmentRange> segmentRanges;

  // Attempt to open the first file as a Zip file.
  struct zip_t *zip = zip_open(cl::InputFilenames[0].data(), 0, 'r');

  if (llvm::sys::fs::is_directory(cl::InputFilenames[0]) || zip) {
    ::hermes::parser::JSONObject *metadata =
        readInputFilenamesFromDirectoryOrZip(
            cl::InputFilenames[0], fileBufs, segmentRanges, metadataAlloc, zip);

    if (zip) {
      zip_close(zip);
    }
    if (!metadata) {
      return InputFileError;
    }

    resolutionTable = readResolutionTable(metadata);
  } else {
    // If we aren't reading from a dir or a zip, we have only one segment.
    Context::SegmentRange range;
    range.first = 0;
    range.last = cl::InputFilenames.size();
    range.segment = 0;
    segmentRanges.push_back(std::move(range));

    SegmentTableEntry entry{};
    for (const std::string &filename : cl::InputFilenames) {
      auto fileBuf = memoryBufferFromFile(filename, true);
      if (!fileBuf)
        return InputFileError;
      entry.push_back({std::move(fileBuf), nullptr});
    }
    fileBufs.emplace(0, std::move(entry));
  }

  if (cl::BytecodeMode) {
    assert(
        fileBufs.size() == 1 && fileBufs[0].size() == 1 &&
        "validateFlags() should enforce exactly one bytecode input file");
    return processBytecodeFile(std::move(fileBufs[0][0].file));
  } else {
    std::shared_ptr<Context> context =
        createContext(std::move(resolutionTable), std::move(segmentRanges));
    return processSourceFiles(context, std::move(fileBufs));
  }
}
} // namespace driver
} // namespace hermes
