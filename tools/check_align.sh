#!/bin/sh
# List word/long memory accesses with an odd displacement (68000 address error risk).
m68k-linux-gnu-objdump -d "$1" | grep -E '^\s+[0-9a-f]+:' | \
  grep -E '\s(move|movea|add|sub|cmp|and|or|eor|tst|clr|neg|not|lea|pea)[wl]?\s' | \
  grep -vE '\s(moveb|addb|subb|cmpb|andb|orb|eorb|tstb|clrb|negb|notb|lea|pea)\s' | \
  grep -E '@\(-?[0-9]*[13579](,|\))' || true
