// RUN: %hermes -O -target=HBC %s | %FileCheck --match-full-lines %s
// Check that exceptions in indexed setters are propagated.
"use strict";

var a = new Int8Array(4);
var o = {
    valueOf: function() { throw Error("Surprise!"); }
}

try {
a[0] = o;
} catch(e) {
    print("caught", e.name, e.message);
}
//CHECK: caught Error Surprise!
