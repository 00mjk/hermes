#ifndef HERMES_VM_JSLIB_RUNTIMECOMMONSTORAGE_H
#define HERMES_VM_JSLIB_RUNTIMECOMMONSTORAGE_H

#include <random>
#if __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#endif
#if defined(HERMESVM_SYNTH_REPLAY) || defined(HERMESVM_API_TRACE)
#include "hermes/VM/MockedEnvironment.h"
#endif

namespace hermes {
namespace vm {

/// This struct provides a shared location for per-Runtime storage needs of
/// JSLib. Runtime owns and provides access to an instance of this class.
struct RuntimeCommonStorage {
  RuntimeCommonStorage();
  ~RuntimeCommonStorage();

  /// RuntimeCommonStorage is tied to a single Runtime, and should not be copied
  RuntimeCommonStorage(const RuntimeCommonStorage &) = delete;
  void operator=(const RuntimeCommonStorage &) = delete;

#ifdef HERMESVM_SYNTH_REPLAY
  /// An environment to replay instead of executing environment-dependent
  /// behavior. This should be used for any circumstance where a result can
  /// change from one run of JS to another.
  MockedEnvironment env;
#endif
#ifdef HERMESVM_API_TRACE
  /// An environment to record environment-dependent behavior (as a sequence of
  /// results of calls to functions).
  MockedEnvironment tracedEnv;
#endif

  /// PRNG used by Math.random()
  std::minstd_rand randomEngine_;
  bool randomEngineSeeded_ = false;

#if __APPLE__
  /// \return a reference to the locale to use for collation, date formatting,
  /// etc. The caller must CFRelease this.
  static CFLocaleRef copyLocale();
#endif
};

} // namespace vm
} // namespace hermes

#endif
