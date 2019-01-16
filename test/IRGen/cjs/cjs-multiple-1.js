// RUN: %hermes -commonjs -dump-ir %S/cjs-multiple-1.js %S/cjs-multiple-2.js | %FileCheck --match-full-lines %s

print('done 1');

// CHECK: function global()
// CHECK-NEXT: frame = []
// CHECK-NEXT: %BB0:
// CHECK-NEXT:   %0 = AllocStackInst $?anon_0_ret
// CHECK-NEXT:   %1 = StoreStackInst undefined : undefined, %0
// CHECK-NEXT:   %2 = TryLoadGlobalPropertyInst globalObject : object, "HermesInternal" : string
// CHECK-NEXT:   %3 = LoadPropertyInst %2, "require" : string
// CHECK-NEXT:   %4 = LoadPropertyInst %3, "call" : string
// CHECK-NEXT:   %5 = CallInst %4, %3, "./" : string, "./cjs-multiple-1.js" : string
// CHECK-NEXT:   %6 = StoreStackInst %5, %0
// CHECK-NEXT:   %7 = LoadStackInst %0
// CHECK-NEXT:   %8 = ReturnInst %7
// CHECK-NEXT: function_end

// CHECK: function cjs_module(exports, require, module)
// CHECK-NEXT: frame = [exports, require, module]
// CHECK-NEXT: %BB0:
// CHECK-NEXT:   %0 = StoreFrameInst %exports, [exports]
// CHECK-NEXT:   %1 = StoreFrameInst %require, [require]
// CHECK-NEXT:   %2 = StoreFrameInst %module, [module]
// CHECK-NEXT:   %3 = TryLoadGlobalPropertyInst globalObject : object, "print" : string
// CHECK-NEXT:   %4 = CallInst %3, undefined : undefined, "done 1" : string
// CHECK-NEXT:   %5 = ReturnInst undefined : undefined
// CHECK-NEXT: function_end

// CHECK: function "cjs_module 1#"(exports, require, module)
// CHECK-NEXT: frame = [exports, require, module]
// CHECK-NEXT: %BB0:
// CHECK-NEXT:   %0 = StoreFrameInst %exports, [exports]
// CHECK-NEXT:   %1 = StoreFrameInst %require, [require]
// CHECK-NEXT:   %2 = StoreFrameInst %module, [module]
// CHECK-NEXT:   %3 = TryLoadGlobalPropertyInst globalObject : object, "print" : string
// CHECK-NEXT:   %4 = CallInst %3, undefined : undefined, "done 2" : string
// CHECK-NEXT:   %5 = ReturnInst undefined : undefined
// CHECK-NEXT: function_end
