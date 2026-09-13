' File: tests/e2e/nested_calls.bas
' Purpose: Demonstrate a FUNCTION returning a string and a SUB with side effects.
' Invariants: EXCL appends an exclamation mark; PRINTDOUBLE prints double of its input.
' Ownership: tests/e2e harness; invoked by CTest.
' Links: docs/examples.md#nested-calls

FUNCTION EXCL$(S$)
  RETURN S$ + "!"
END FUNCTION
FUNCTION DOUBLE(N)
  RETURN N * 2
END FUNCTION
SUB PRINTDOUBLE(X)
  PRINT DOUBLE(X)
END SUB
10 PRINT EXCL$("hi")
20 PRINTDOUBLE(21)
30 END
