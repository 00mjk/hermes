// RUN: %hermes -O -target=HBC %s | %FileCheck --match-full-lines %s

print(10 % 2);
//CHECK: 0

print(10 % 3);
//CHECK: 1

print(10 % 0);
//CHECK: NaN

print(10 % Infinity);
//CHECK: 10

print(10 % NaN);
//CHECK: NaN

print(0 % 10);
//CHECK: 0

print(Infinity % 10);
//CHECK: NaN

print(NaN % 10);
//CHECK: NaN

print(5.5 % 1.5);
//CHECK: 1

print(5.5 % -1.5);
//CHECK: 1

print(-5.5 % 1.5);
//CHECK: -1

print(-5.5 % -1.5);
//CHECK: -1

print(5.5 % 2.5);
//CHECK: 0.5

print(5.5 % -2.5);
//CHECK: 0.5

print(-5.5 % 2.5);
//CHECK: -0.5

print(-5.5 % -2.5);
//CHECK: -0.5

