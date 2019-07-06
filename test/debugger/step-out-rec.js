// Copyright (c) Facebook, Inc. and its affiliates.
//
// This source code is licensed under the MIT license found in the LICENSE
// file in the root directory of this source tree.
//
// RUN: %hdb %s < %s.debug | %FileCheck --match-full-lines %s
// REQUIRES: debugger

function fact(n) {
  if (n < 2) {
    return 1;
  }
  return n * fact(n - 1);
}

debugger;
fact(4);

// CHECK: Break on 'debugger' statement in global: {{.*}}:16:1
// CHECK-NEXT: Stepped to global: {{.*}}:17:1
// CHECK-NEXT: Stepped to fact: {{.*}}:10:7
// CHECK-NEXT: Stepped to global: {{.*}}:17:5
// CHECK-NEXT: Continuing execution
