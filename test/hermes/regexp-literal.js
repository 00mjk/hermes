// RUN: %hermes -O -target=HBC %s | %FileCheck --match-full-lines %s
// RUN: %hermes -O -target=HBC -emit-binary -out %t.hbc %s && %hermes %t.hbc | %FileCheck --match-full-lines %s
// Make sure RegExp literals.
"use strict";

print('RegExp Literal');
// CHECK-LABEL: RegExp Literal
var re = /aa/;
print(re.__proto__.constructor === RegExp);
// CHECK-NEXT: true
print(/abc/.toString());
// CHECK-NEXT: /abc/

var m = /\w (\d+)/.exec("abc 1234");
print(m.input + ", " + m.index + ", " + m.length + ", " + m[1]);
// CHECK-NEXT: abc 1234, 2, 2, 1234
print(/\w (\d+)/.exec("abc def"));
// CHECK-NEXT: null
print(/\w (\d+)/.exec("abc 1234"));
// CHECK-NEXT: c 1234,1234

// Ensure that escapes are not interpreted in regexp flags.
try { print(/a/\u0067); } catch(e) { print('Caught', e.name); }
// CHECK-NEXT: Caught SyntaxError
