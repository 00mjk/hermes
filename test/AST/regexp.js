// RUN: (! %hermes %s 2>&1 ) | %FileCheck %s

print(/[b-a]/);
//CHECK: {{.*}}regexp.js:3:7: error: Invalid regular expression: Character class range out of order
//CHECK-NEXT: print(/[b-a]/);
//CHECK-NEXT:       ^~~~~~~

print(/(abc/);
//CHECK: {{.*}}regexp.js:8:7: error: Invalid regular expression: Parenthesized expression not closed
//CHECK-NEXT: print(/(abc/);
//CHECK-NEXT:       ^~~~~~

print(/.{5,3}/);
//CHECK: {{.*}}regexp.js:13:7: error: Invalid regular expression: Quantifier range out of order
//CHECK-NEXT: print(/.{5,3}/);
//CHECK-NEXT:       ^~~~~~~~

print(/{100}/);
//CHECK: {{.*}}regexp.js:18:7: error: Invalid regular expression: Quantifier has nothing to repeat
//CHECK-NEXT: print(/{100}/);
//CHECK-NEXT:       ^~~~~~~
