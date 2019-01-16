// RUN: %hermes -dump-bytecode -target=HBC -g %s | %FileCheck %s --match-full-lines
// RUN: cp %s %T/debug_info_à.js && %hermes -dump-bytecode -target=HBC -g %T/debug_info_à.js | %FileCheck %T/debug_info_à.js --match-full-lines --check-prefix=UNICODE

var v1g = "global";

function Fa() {
    var v1a = 3;
    var v2a = 5;
}
// CHECK: Function<Fa>{{.*}}
// CHECK-NEXT: Offset in debug table: {{.*}}
// UNICODE: Function<Fa>{{.*}}
// UNICODE-NEXT: Offset in debug table: {{.*}}


function Fb() {
    var v1b = "abc";
}
// CHECK: Function<Fb>{{.*}}
// CHECK-NEXT: Offset in debug table: {{.*}}
// UNICODE: Function<Fb>{{.*}}
// UNICODE-NEXT: Offset in debug table: {{.*}}

function Fc() {
    var v1c = undefined;
    function Fcc() {
        var v1cc = 42;
    }
}
// CHECK: Function<Fc>{{.*}}
// CHECK-NEXT: Offset in debug table: {{.*}}
// CHECK: Function<Fcc>(1 params, 4 registers, 1 symbols):
// CHECK-NEXT: Offset in debug table: {{.*}}
// UNICODE: Function<Fc>{{.*}}
// UNICODE-NEXT: Offset in debug table: {{.*}}
// UNICODE: Function<Fcc>(1 params, 4 registers, 1 symbols):
// UNICODE-NEXT: Offset in debug table: {{.*}}

// CHECK:      Debug filename table:
// CHECK-NEXT:   0: {{.*}}/debug_info.js
// UNICODE:      Debug filename table:
// UNICODE-NEXT:   0: {{.*}}/debug_info_à.js

// CHECK:      Debug file table:
// CHECK-NEXT:   Debug offset 0: string id 0
// UNICODE:      Debug file table:
// UNICODE-NEXT:   Debug offset 0: string id 0

// We expect 5 DebugOffset lines since we have 4 functions plus the global.
// CHECK:      Debug data table:
// CHECK-NEXT:   DebugOffset {{.*}}
// CHECK-NEXT:   DebugOffset {{.*}}
// CHECK-NEXT:   DebugOffset {{.*}}
// CHECK-NEXT:   DebugOffset {{.*}}
// CHECK-NEXT:   DebugOffset {{.*}}
// CHECK-NEXT:   Debug table ends at debugOffset {{.*}}
// CHECK-NEXT: Debug variables table:
// CHECK-NEXT:   Offset: 0x0, vars count: 0, lexical parent: none
// CHECK-NEXT:   Offset: 0x8, vars count: 2, lexical parent: 0
// CHECK-NEXT:     0x0010: "v1a"
// CHECK-NEXT:     0x0017: "v2a"
// CHECK-NEXT:   Offset: 0x1e, vars count: 1, lexical parent: 0
// CHECK-NEXT:     0x0026: "v1b"
// CHECK-NEXT:   Offset: 0x2d, vars count: 2, lexical parent: 0
// CHECK-NEXT:     0x0035: "v1c"
// CHECK-NEXT:     0x003c: "Fcc"
// CHECK-NEXT:   Offset: 0x43, vars count: 1, lexical parent: 3
// CHECK-NEXT:     0x004b: "v1cc"

// UNICODE:      Debug data table:
// UNICODE-NEXT:   DebugOffset {{.*}}
// UNICODE-NEXT:   DebugOffset {{.*}}
// UNICODE-NEXT:   DebugOffset {{.*}}
// UNICODE-NEXT:   DebugOffset {{.*}}
// UNICODE-NEXT:   DebugOffset {{.*}}
// UNICODE-NEXT:   Debug table ends at debugOffset {{.*}}
// UNICODE-NEXT: Debug variables table:
// UNICODE-NEXT:   Offset: 0x0, vars count: 0, lexical parent: none
// UNICODE-NEXT:   Offset: 0x8, vars count: 2, lexical parent: 0
// UNICODE-NEXT:     0x0010: "v1a"
// UNICODE-NEXT:     0x0017: "v2a"
// UNICODE-NEXT:   Offset: 0x1e, vars count: 1, lexical parent: 0
// UNICODE-NEXT:     0x0026: "v1b"
// UNICODE-NEXT:   Offset: 0x2d, vars count: 2, lexical parent: 0
// UNICODE-NEXT:     0x0035: "v1c"
// UNICODE-NEXT:     0x003c: "Fcc"
// UNICODE-NEXT:   Offset: 0x43, vars count: 1, lexical parent: 3
// UNICODE-NEXT:     0x004b: "v1cc"
