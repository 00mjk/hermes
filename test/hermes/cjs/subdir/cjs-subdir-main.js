// RUN: %hermes -commonjs %S/cjs-subdir-main.js %S/cjs-subdir-2.js %S/foo/cjs-subdir-foo.js %S/bar/cjs-subdir-bar.js | %FileCheck --match-full-lines %s
// RUN: %hermes -O -fstatic-builtins -fstatic-require -commonjs %S/cjs-subdir-main.js %S/cjs-subdir-2.js %S/foo/cjs-subdir-foo.js %S/bar/cjs-subdir-bar.js | %FileCheck --match-full-lines %s
// RUN: %hermes -commonjs %S | %FileCheck --match-full-lines %s
// RUN: %hermes -commonjs %S -O -fstatic-require -fstatic-builtins | %FileCheck --match-full-lines %s
// RUN: cd %S && zip %T/subdir.zip metadata.json cjs-subdir-main.js cjs-subdir-2.js foo/cjs-subdir-foo.js bar/cjs-subdir-bar.js && %hermes -commonjs %T/subdir.zip | %FileCheck --match-full-lines %s

print('main: init');
// CHECK-LABEL: main: init

var foo = require('./foo/cjs-subdir-foo.js');
// CHECK-NEXT: foo: init
// CHECK-NEXT: bar: init
// CHECK-NEXT: foo: bar.y = 15

print('main: foo.x =', foo.x);
// CHECK-NEXT: main: foo.x = 15

print(require('./cjs-subdir-2.js').alpha);
// CHECK-NEXT: 2: init
// CHECK-NEXT: 144
