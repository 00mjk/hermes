// Copyright (c) Facebook, Inc. and its affiliates.
//
// This source code is licensed under the MIT license found in the LICENSE
// file in the root directory of this source tree.
//
// RUN: %hdb < %s.debug %s | %FileCheck --match-full-lines %s
// REQUIRES: debugger

print('foo');
// CHECK: foo
