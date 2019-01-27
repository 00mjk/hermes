// RUN: %hermesc -Xflow-parser -dump-ir %s | %FileCheck --match-full-lines %s
// REQUIRES: flowparser

var func1 = () => 10;
//CHECK-LABEL:arrow func1()
//CHECK-NEXT:frame = []
//CHECK-NEXT:%BB0:
//CHECK-NEXT:  %0 = ReturnInst 10 : number
//CHECK-NEXT:%BB1:
//CHECK-NEXT:  %1 = ReturnInst undefined : undefined
//CHECK-NEXT:function_end

var func2 = () => { return 11; }
//CHECK-LABEL:arrow func2()
//CHECK-NEXT:frame = []
//CHECK-NEXT:%BB0:
//CHECK-NEXT:  %0 = ReturnInst 11 : number
//CHECK-NEXT:%BB1:
//CHECK-NEXT:  %1 = ReturnInst undefined : undefined
//CHECK-NEXT:function_end
