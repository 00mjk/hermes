// RUN: %hermes -commonjs -dump-bytecode %S/cjs-multiple-1.js %S/cjs-multiple-2.js | %FileCheck --match-full-lines %s
// RUN: %hermes -O -commonjs -dump-bytecode %S/cjs-multiple-1.js %S/cjs-multiple-2.js | %FileCheck --match-full-lines %s
// RUN: %hermes -O -fstatic-require -fstatic-builtins -commonjs -dump-bytecode %S/cjs-multiple-1.js %S/cjs-multiple-2.js | %FileCheck --match-full-lines %s --check-prefix=STATIC

print('module 1');

// CHECK: CommonJS module count: 2
// CHECK-NEXT: CommonJS module count (static): 0

// CHECK: Global String Table:
// CHECK: s{{.*}}[ASCII, {{.*}}]: cjs-multiple-1.js
// CHECK: s{{.*}}[ASCII, {{.*}}]: cjs-multiple-2.js
// CHECK-NOT: cjs-multiple-1
// CHECK: CommonJS Modules:
// CHECK-NEXT:   File ID {{.*}} -> function ID 1
// CHECK-NEXT:   File ID {{.*}} -> function ID 2
// CHECK: Debug filename table:
// CHECK-NEXT:   0: {{.*}}/cjs-multiple-1.js
// CHECK-NEXT:   1: {{.*}}/cjs-multiple-2.js

// STATIC: CommonJS module count: 0
// STATIC-NEXT: CommonJS module count (static): 2

// STATIC: Global String Table:
// STATIC-NEXT: s0[ASCII, {{.*}}]: global
// STATIC-NEXT: s1[ASCII, {{.*}}]: cjs_module
// STATIC-NEXT: s2[ASCII, {{.*}}]: module 1
// STATIC-NEXT: i3[ASCII, {{.*}}] #{{[0-9A-Z]+}}: print
// STATIC-NEXT: s4[ASCII, {{.*}}]: module 2
// STATIC-NOT: cjs-multiple-1
// STATIC: CommonJS Modules (Static):
// STATIC-NEXT:  Module index 0 -> function ID 1
// STATIC-NEXT:  Module index 1 -> function ID 2
// STATIC: Debug filename table:
// STATIC-NEXT:   0: {{.*}}/cjs-multiple-1.js
// STATIC-NEXT:   1: {{.*}}/cjs-multiple-2.js
