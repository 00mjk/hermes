// RUN: %hermes -O -Xes6-symbol %s | %FileCheck --match-full-lines %s
"use strict";

// Runs an iterator until it is done.
// Prints out `value` and `done` every iteration.
function runIterator(it) {
  while (true) {
    var result = it.next();
    print(result.value, result.done);
    if (result.done) return;
  }
}

print('IteratorPrototype');
// CHECK-LABEL: IteratorPrototype
print(Object.getPrototypeOf(
  Object.getPrototypeOf([].keys())
)[Symbol.iterator].name);
// CHECK-LABEL: [Symbol.iterator]

print('Array Iterator');
// CHECK-LABEL: Array Iterator
var a = ['a','b','c'];
runIterator(a.keys());
// CHECK-NEXT: 0 false
// CHECK-NEXT: 1 false
// CHECK-NEXT: 2 false
// CHECK-NEXT: undefined true
runIterator(a.values());
// CHECK-NEXT: a false
// CHECK-NEXT: b false
// CHECK-NEXT: c false
// CHECK-NEXT: undefined true
runIterator(a.entries());
// CHECK-NEXT: 0,a false
// CHECK-NEXT: 1,b false
// CHECK-NEXT: 2,c false
// CHECK-NEXT: undefined true
var a = [];
runIterator(a.keys());
// CHECK-NEXT: undefined true
runIterator(a.values());
// CHECK-NEXT: undefined true
runIterator(a.entries());
// CHECK-NEXT: undefined true

print('TypedArray Iterator');
// CHECK-LABEL: TypedArray Iterator
var a = new Uint8Array([10, 11, 12]);
runIterator(a.keys());
// CHECK-NEXT: 0 false
// CHECK-NEXT: 1 false
// CHECK-NEXT: 2 false
// CHECK-NEXT: undefined true
runIterator(a.values());
// CHECK-NEXT: 10 false
// CHECK-NEXT: 11 false
// CHECK-NEXT: 12 false
// CHECK-NEXT: undefined true
runIterator(a.entries());
// CHECK-NEXT: 0,10 false
// CHECK-NEXT: 1,11 false
// CHECK-NEXT: 2,12 false
// CHECK-NEXT: undefined true
var a = new Uint8Array([]);
runIterator(a.keys());
// CHECK-NEXT: undefined true
runIterator(a.values());
// CHECK-NEXT: undefined true
runIterator(a.entries());
// CHECK-NEXT: undefined true

print('String Iterator');
// CHECK-LABEL: String Iterator
var a = 'abcd';
runIterator(a[Symbol.iterator]());
// CHECK-NEXT: a false
// CHECK-NEXT: b false
// CHECK-NEXT: c false
// CHECK-NEXT: d false
// CHECK-NEXT: undefined true
var a = '';
runIterator(a[Symbol.iterator]());
// CHECK-NEXT: undefined true
var a = 'x\uD83D\uDCD3y';
runIterator(a[Symbol.iterator]());
// CHECK-NEXT: x false
// CHECK-NEXT: 📓 false
// CHECK-NEXT: y false
// CHECK-NEXT: undefined true
