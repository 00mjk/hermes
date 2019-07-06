// Copyright (c) Facebook, Inc. and its affiliates.
//
// This source code is licensed under the MIT license found in the LICENSE
// file in the root directory of this source tree.
//
// RUN: %hermes -hermes-parser -dump-ir %s | %FileCheck --match-full-lines %s
// RUN: %hermes -hermes-parser -dump-ir %s -O

var x = {
    1: 10,
    get a() { return "a" },
    get 1() { return 20; },
    b: 11,
    set 1(x) {},
    get 1() { return 21; },
    b: 12,
}

//CHECK-LABEL:function global()
//CHECK-NEXT:frame = [], globals = [x]
//CHECK-NEXT:  %BB0:
//CHECK-NEXT:  %0 = AllocStackInst $?anon_0_ret
//CHECK-NEXT:  %1 = StoreStackInst undefined : undefined, %0
//CHECK-NEXT:  %2 = AllocObjectInst 3 : number, empty
//CHECK-NEXT:  %3 = CreateFunctionInst %"get a"()
//CHECK-NEXT:  %4 = StoreGetterSetterInst %3 : closure, undefined : undefined, %2 : object, "a" : string, true : boolean
//CHECK-NEXT:  %5 = CreateFunctionInst %"get 1"()
//CHECK-NEXT:  %6 = CreateFunctionInst %"set 1"()
//CHECK-NEXT:  %7 = StoreGetterSetterInst %5 : closure, %6 : closure, %2 : object, "1" : string, true : boolean
//CHECK-NEXT:  %8 = StoreNewOwnPropertyInst 12 : number, %2 : object, "b" : string, true : boolean
//CHECK-NEXT:  %9 = StorePropertyInst %2 : object, globalObject : object, "x" : string
//CHECK-NEXT:  %10 = LoadStackInst %0
//CHECK-NEXT:  %11 = ReturnInst %10
//CHECK-NEXT:function_end

//CHECK-LABEL:function "get a"()
//CHECK-NEXT:frame = []
//CHECK-NEXT:  %BB0:
//CHECK-NEXT:    %0 = ReturnInst "a" : string
//CHECK-NEXT:  %BB1:
//CHECK-NEXT:    %1 = ReturnInst undefined : undefined
//CHECK-NEXT:function_end

//CHECK-LABEL:function "get 1"()
//CHECK-NEXT:frame = []
//CHECK-NEXT:  %BB0:
//CHECK-NEXT:    %0 = ReturnInst 21 : number
//CHECK-NEXT:  %BB1:
//CHECK-NEXT:    %1 = ReturnInst undefined : undefined
//CHECK-NEXT:function_end

//CHECK-LABEL:function "set 1"(x)
//CHECK-NEXT:frame = [x]
//CHECK-NEXT:  %BB0:
//CHECK-NEXT:    %0 = StoreFrameInst %x, [x]
//CHECK-NEXT:    %1 = ReturnInst undefined : undefined
//CHECK-NEXT:function_end
