// RUN: %hermes -commonjs %S/ -emit-binary -out %T/test.hbc && %hermes %T/test.hbc | %FileCheck --match-full-lines %s
// RUN: %hermes -commonjs -fstatic-require -fstatic-builtins %S/ -emit-binary -out %T/test.hbc && %hermes %T/test.hbc | %FileCheck --match-full-lines %s
// RUN: %hermes -O -commonjs -fstatic-require -fstatic-builtins %S/ -emit-binary -out %T/test.hbc && %hermes %T/test.hbc | %FileCheck --match-full-lines %s
// RUN: %hermes -O -commonjs -fstatic-require -fstatic-builtins %S/ -emit-binary -out %T/test.hbc && %hermes -b -dump-bytecode %T/test.hbc | %FileCheck --match-full-lines %s -check-prefix BC

print('main: init');
// CHECK-LABEL: main: init

loadSegment(require.context, 1);

var foo = require('./foo/cjs-subdir-foo.js');
// CHECK-NEXT: foo: init
// CHECK-NEXT: bar: init
// CHECK-NEXT: foo: bar.y = 15

print('main: foo.x =', foo.x);
// CHECK-NEXT: main: foo.x = 15

print(require('./cjs-subdir-2.js').alpha);
// CHECK-NEXT: 2: init
// CHECK-NEXT: 144

// BC-LABEL: String count: 10
// BC-LABEL: Global String Table:
// BC-NEXT:   s0[ASCII, {{.*}}]: main: init
// BC-NEXT:   s1[ASCII, {{.*}}]: main: foo.x =
// BC-NEXT:   s2[ASCII, {{.*}}]: global
// BC-NEXT:   s3[ASCII, {{.*}}]: 2: init
// BC-NEXT:   s4[ASCII, {{.*}}]: cjs_module
// BC-NEXT:   i5[ASCII, {{.*}}] #{{.*}}: loadSegment
// BC-NEXT:   i6[ASCII, {{.*}}] #{{.*}}: x
// BC-NEXT:   i7[ASCII, {{.*}}] #{{.*}}: alpha
// BC-NEXT:   i8[ASCII, {{.*}}] #{{.*}}: print
// BC-NEXT:   i9[ASCII, {{.*}}] #{{.*}}: context
