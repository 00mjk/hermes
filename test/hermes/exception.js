// RUN: %hermes -O -target=HBC %s | %FileCheck --match-full-lines %s

try {
  throw "test";
} catch (e) {
  print(e);
}
// CHECK: test

function foo() {
  throw "foo error";
}
function bar() {
  foo();
}
function cee() {
  try {
    bar();
  } catch (e) {
    return e;
  }
}
print(cee());
// CHECK: foo error

try {
  var x = undefined;
  x.bad;
} catch (e) {
  print(e);
}
// CHECK: TypeError: Cannot read property 'bad' of undefined


// A few complicated cases:

try{
  throw "ex1";
}
catch(er1){
  try{
    throw "ex2";
  }
  catch(er1){
    print(er1);
    if (er1!=="ex2") print(er1);
  }
}
//CHECK: ex2

try{
  throw "ex1";
}
catch(er1){
  print(er1);
}

finally{
  try{
    throw "ex2";
  }
  catch(er1){
  }
}
//CHECK: ex1

