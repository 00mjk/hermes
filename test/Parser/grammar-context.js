// RUN: %hermesc -dump-ast -pretty-json %s | %FileCheck --match-full-lines %s

// Make sure we distinguish between div and regexp correctly.

//CHECK:      {
//CHECK-NEXT:   "type": "File",
//CHECK-NEXT:   "program": {
//CHECK-NEXT:     "type": "Program",
//CHECK-NEXT:     "body": [

(function(){return 1} / {});
//CHECK-NEXT:       {
//CHECK-NEXT:         "type": "ExpressionStatement",
//CHECK-NEXT:         "expression": {
//CHECK-NEXT:           "type": "BinaryExpression",
//CHECK-NEXT:           "left": {
//CHECK-NEXT:             "type": "FunctionExpression",
//CHECK-NEXT:             "id": null,
//CHECK-NEXT:             "params": [],
//CHECK-NEXT:             "body": {
//CHECK-NEXT:               "type": "BlockStatement",
//CHECK-NEXT:               "body": [
//CHECK-NEXT:                 {
//CHECK-NEXT:                   "type": "ReturnStatement",
//CHECK-NEXT:                   "argument": {
//CHECK-NEXT:                     "type": "NumericLiteral",
//CHECK-NEXT:                     "value": 1
//CHECK-NEXT:                   }
//CHECK-NEXT:                 }
//CHECK-NEXT:               ]
//CHECK-NEXT:             }
//CHECK-NEXT:           },
//CHECK-NEXT:           "right": {
//CHECK-NEXT:             "type": "ObjectExpression",
//CHECK-NEXT:             "properties": []
//CHECK-NEXT:           },
//CHECK-NEXT:           "operator": "\/"
//CHECK-NEXT:         },
//CHECK-NEXT:         "directive": null
//CHECK-NEXT:       },

/a/;
//CHECK-NEXT:       {
//CHECK-NEXT:         "type": "ExpressionStatement",
//CHECK-NEXT:         "expression": {
//CHECK-NEXT:           "type": "RegExpLiteral",
//CHECK-NEXT:           "pattern": "a",
//CHECK-NEXT:           "flags": ""
//CHECK-NEXT:         },
//CHECK-NEXT:         "directive": null
//CHECK-NEXT:       }


//CHECK-NEXT:     ]
//CHECK-NEXT:   }
//CHECK-NEXT: }
