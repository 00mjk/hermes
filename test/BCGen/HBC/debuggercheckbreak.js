// RUN: %hermes -dump-bytecode -target=HBC %s -O -g | %FileCheck %s --match-full-lines

//CHECK-LABEL:Function<global>{{.*}}:
//CHECK-NEXT:Offset in debug table: src 0x0, vars 0x0
//CHECK-NEXT:    DeclareGlobalVar  "test1"
//CHECK-NEXT:    CreateEnvironment r0
//CHECK-NEXT:    CreateClosure     r1, r0, 1
//CHECK-NEXT:    GetGlobalObject   r0
//CHECK-NEXT:    PutById           r0, r1, 1, "test1"
//CHECK-NEXT:    LoadConstUndefined r0
//CHECK-NEXT:    DebuggerCheckBreak
//CHECK-NEXT:    Ret               r0

//CHECK-LABEL:Function<test1>(1 params, 17 registers, 0 symbols):
//CHECK-NEXT:Offset in debug table: {{.*}}
//CHECK-NEXT:    GetGlobalObject   r0
//CHECK-NEXT:    LoadConstUInt8    r5, 3
//CHECK-NEXT:    LoadConstUInt8    r4, 1
//CHECK-NEXT:    LoadConstUInt8    r1, 5
//CHECK-NEXT:    LoadConstUInt8    r3, 10
//CHECK-NEXT:    LoadConstZero     r6
//CHECK-NEXT:    DebuggerCheckBreak
//CHECK-NEXT:L3:
//CHECK-NEXT:    TryGetById        r7, r0, 1, "Math"
//CHECK-NEXT:    GetByIdShort      r2, r7, 2, "random"
//CHECK-NEXT:    Mov               r10, r7
//CHECK-NEXT:    Call              r7, r2, 1
//CHECK-NEXT:    Mov               r2, r6
//CHECK-NEXT:    DebuggerCheckBreak
//CHECK-NEXT:    JStrictEqual      L1, r7, r5
//CHECK-NEXT:    TryGetById        r8, r0, 1, "Math"
//CHECK-NEXT:    GetByIdShort      r7, r8, 2, "random"
//CHECK-NEXT:    Mov               r10, r8
//CHECK-NEXT:    Call              r7, r7, 1
//CHECK-NEXT:    JStrictEqual      L2, r7, r1
//CHECK-NEXT:    AddN              r6, r2, r4
//CHECK-NEXT:    Jmp               L3
//CHECK-NEXT:L2:
//CHECK-NEXT:    DebuggerCheckBreak
//CHECK-NEXT:    Jmp               L2
//CHECK-NEXT:L1:
//CHECK-NEXT:    Mov               r1, r2
//CHECK-NEXT:    Mov               r2, r1
//CHECK-NEXT:    JNotGreaterN      L4, r2, r3
//CHECK-NEXT:L5:
//CHECK-NEXT:    SubN              r1, r1, r4
//CHECK-NEXT:    Mov               r2, r1
//CHECK-NEXT:    DebuggerCheckBreak
//CHECK-NEXT:    JGreaterN         L5, r2, r3
//CHECK-NEXT:L4:
//CHECK-NEXT:    TryGetById        r1, r0, 3, "print"
//CHECK-NEXT:    LoadConstUndefined r0
//CHECK-NEXT:    LoadConstUndefined r10
//CHECK-NEXT:    Mov               r9, r2
//CHECK-NEXT:    Call              r1, r1, 2
//CHECK-NEXT:    Ret               r0

function test1() {
  var count = 0;
  for(var count=0; ; count++) {
    if (Math.random() === 3)
      break;
    if (Math.random() === 5)
      for(;;){} // infinite loop
  }
  while (count > 10)
    count--;
  print(count);
}
