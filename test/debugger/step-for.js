// Copyright (c) Facebook, Inc. and its affiliates.
//
// This source code is licensed under the MIT license found in the LICENSE
// file in the root directory of this source tree.
//
// RUN: %hdb < %s.debug %s | %FileCheck --match-full-lines %s
// REQUIRES: debugger

debugger;
for (var i = 0; i < 5; ++i) {
  print('iteration', i);
}

// CHECK: Break on 'debugger' statement in global: {{.*}}:9:1
// CHECK-NEXT: Stepped to global: {{.*}}:10:12
// CHECK-NEXT: Stepped to global: {{.*}}:11:3
// CHECK-NEXT: iteration 0
// CHECK-NEXT: Stepped to global: {{.*}}:10:26
// CHECK-NEXT: Stepped to global: {{.*}}:11:3
// CHECK-NEXT: iteration 1
// CHECK-NEXT: Stepped to global: {{.*}}:10:26
// CHECK-NEXT: Stepped to global: {{.*}}:11:3
// CHECK-NEXT: iteration 2
// CHECK-NEXT: Stepped to global: {{.*}}:10:26
// CHECK-NEXT: Stepped to global: {{.*}}:11:3
// CHECK-NEXT: iteration 3
// CHECK-NEXT: Stepped to global: {{.*}}:10:26
// CHECK-NEXT: Stepped to global: {{.*}}:11:3
// CHECK-NEXT: iteration 4
