#ifndef HERMES_VM_GCDECL_H
#define HERMES_VM_GCDECL_H

namespace hermes {
namespace vm {

#if defined(HERMESVM_GC_MALLOC)
class MallocGC;
using GC = MallocGC;
#elif defined(HERMESVM_GC_GENERATIONAL)
class GenGC;
using GC = GenGC;
#elif defined(HERMESVM_GC_NONCONTIG_GENERATIONAL)
class GenGC;
using GC = GenGC;
#else
#error "Unsupported HermesVM GCKIND" #HERMESVM_GCKIND
#endif

} // namespace vm
} // namespace hermes

#endif // HERMES_VM_GCDECL_H
