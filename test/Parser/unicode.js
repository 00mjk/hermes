// RUN: %hermes -hermes-parser -dump-ir %s

// Ensure Unicode characters are recognized correctly in identifiers.

// UnicodeCombiningMark
var a͂b͎c̊ = false;

// UnicodeDigit
var D٠٩۰۹߀߉०९০৫੩૧૬୨௧௮౪09೨൬๔０５９ = true;
// UnicodeConnectorPunctuation
var z_‿⁀⁔︳︴﹍﹎﹏＿ = 1.0;

// Put them all together.
{Ⅵ૬͓͋﹍͕͔: "𐒡"};
