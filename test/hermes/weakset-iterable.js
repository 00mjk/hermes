// RUN: %hermes -O -Xes6-symbol %s | %FileCheck --match-full-lines %s

var iterable = {};
iterable[Symbol.iterator] = function() {
  return {
    next: function() {
      return {value: 1, done: false};
    },
    return: function() {
      print('returning');
    },
  };
};
var oldAdd = WeakSet.prototype.add;
WeakSet.prototype.add = function() {
  throw new Error('add error');
}
try { new WeakSet(iterable); } catch (e) { print('caught', e.message); }
// CHECK: returning
// CHECK: caught add error
WeakSet.prototype.add = oldAdd;
