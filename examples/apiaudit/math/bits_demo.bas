' =============================================================================
' API Audit: Zanna.Math.Bits - Bitwise Operations (BASIC)
' =============================================================================
' Tests: And, Or, Xor, Not, Shl, Shr, ShiftRightLogical, Set, Clear, Get,
'        Toggle, Flip, Count, CountLeadingZeros, CountTrailingZeros,
'        RotateLeft, RotateRight, Swap
' =============================================================================

PRINT "=== API Audit: Zanna.Math.Bits ==="

' --- And ---
PRINT "--- And ---"
PRINT "Bits.And(12, 10): "; Zanna.Math.Bits.And(12, 10)
PRINT "Bits.And(255, 15): "; Zanna.Math.Bits.And(255, 15)
PRINT "Bits.And(0, 0): "; Zanna.Math.Bits.And(0, 0)

' --- Or ---
PRINT "--- Or ---"
PRINT "Bits.Or(12, 10): "; Zanna.Math.Bits.Or(12, 10)
PRINT "Bits.Or(0, 0): "; Zanna.Math.Bits.Or(0, 0)
PRINT "Bits.Or(240, 15): "; Zanna.Math.Bits.Or(240, 15)

' --- Xor ---
PRINT "--- Xor ---"
PRINT "Bits.Xor(12, 10): "; Zanna.Math.Bits.Xor(12, 10)
PRINT "Bits.Xor(255, 255): "; Zanna.Math.Bits.Xor(255, 255)
PRINT "Bits.Xor(0, 0): "; Zanna.Math.Bits.Xor(0, 0)

' --- Not ---
PRINT "--- Not ---"
PRINT "Bits.Not(0): "; Zanna.Math.Bits.Not(0)
PRINT "Bits.Not(1): "; Zanna.Math.Bits.Not(1)
PRINT "Bits.Not(255): "; Zanna.Math.Bits.Not(255)

' --- Shl ---
PRINT "--- Shl ---"
PRINT "Bits.Shl(1, 0): "; Zanna.Math.Bits.Shl(1, 0)
PRINT "Bits.Shl(1, 4): "; Zanna.Math.Bits.Shl(1, 4)
PRINT "Bits.Shl(1, 8): "; Zanna.Math.Bits.Shl(1, 8)
PRINT "Bits.Shl(3, 2): "; Zanna.Math.Bits.Shl(3, 2)

' --- Shr ---
PRINT "--- Shr ---"
PRINT "Bits.Shr(16, 2): "; Zanna.Math.Bits.Shr(16, 2)
PRINT "Bits.Shr(256, 4): "; Zanna.Math.Bits.Shr(256, 4)
PRINT "Bits.Shr(1, 0): "; Zanna.Math.Bits.Shr(1, 0)

' --- ShiftRightLogical ---
PRINT "--- ShiftRightLogical ---"
PRINT "Bits.ShiftRightLogical(16, 2): "; Zanna.Math.Bits.ShiftRightLogical(16, 2)
PRINT "Bits.ShiftRightLogical(-1, 60): "; Zanna.Math.Bits.ShiftRightLogical(-1, 60)

' --- Set ---
PRINT "--- Set ---"
PRINT "Bits.Set(0, 0): "; Zanna.Math.Bits.Set(0, 0)
PRINT "Bits.Set(0, 3): "; Zanna.Math.Bits.Set(0, 3)
PRINT "Bits.Set(1, 1): "; Zanna.Math.Bits.Set(1, 1)

' --- Clear ---
PRINT "--- Clear ---"
PRINT "Bits.Clear(255, 0): "; Zanna.Math.Bits.Clear(255, 0)
PRINT "Bits.Clear(255, 7): "; Zanna.Math.Bits.Clear(255, 7)
PRINT "Bits.Clear(8, 3): "; Zanna.Math.Bits.Clear(8, 3)

' --- Get ---
PRINT "--- Get ---"
PRINT "Bits.Get(5, 0): "; Zanna.Math.Bits.Get(5, 0)
PRINT "Bits.Get(5, 1): "; Zanna.Math.Bits.Get(5, 1)
PRINT "Bits.Get(5, 2): "; Zanna.Math.Bits.Get(5, 2)

' --- Toggle ---
PRINT "--- Toggle ---"
PRINT "Bits.Toggle(0, 0): "; Zanna.Math.Bits.Toggle(0, 0)
PRINT "Bits.Toggle(1, 0): "; Zanna.Math.Bits.Toggle(1, 0)
PRINT "Bits.Toggle(5, 1): "; Zanna.Math.Bits.Toggle(5, 1)

' --- Flip ---
PRINT "--- Flip ---"
PRINT "Bits.Flip(0): "; Zanna.Math.Bits.Flip(0)
PRINT "Bits.Flip(1): "; Zanna.Math.Bits.Flip(1)
PRINT "Bits.Flip(255): "; Zanna.Math.Bits.Flip(255)

' --- Count ---
PRINT "--- Count ---"
PRINT "Bits.CountOnes(0): "; Zanna.Math.Bits.CountOnes(0)
PRINT "Bits.CountOnes(1): "; Zanna.Math.Bits.CountOnes(1)
PRINT "Bits.CountOnes(7): "; Zanna.Math.Bits.CountOnes(7)
PRINT "Bits.CountOnes(255): "; Zanna.Math.Bits.CountOnes(255)

' --- CountLeadingZeros ---
PRINT "--- CountLeadingZeros ---"
PRINT "Bits.CountLeadingZeros(1): "; Zanna.Math.Bits.CountLeadingZeros(1)
PRINT "Bits.CountLeadingZeros(256): "; Zanna.Math.Bits.CountLeadingZeros(256)

' --- CountTrailingZeros ---
PRINT "--- CountTrailingZeros ---"
PRINT "Bits.CountTrailingZeros(1): "; Zanna.Math.Bits.CountTrailingZeros(1)
PRINT "Bits.CountTrailingZeros(8): "; Zanna.Math.Bits.CountTrailingZeros(8)
PRINT "Bits.CountTrailingZeros(16): "; Zanna.Math.Bits.CountTrailingZeros(16)

' --- RotateLeft ---
PRINT "--- RotateLeft ---"
PRINT "Bits.RotateLeft(1, 1): "; Zanna.Math.Bits.RotateLeft(1, 1)
PRINT "Bits.RotateLeft(1, 4): "; Zanna.Math.Bits.RotateLeft(1, 4)

' --- RotateRight ---
PRINT "--- RotateRight ---"
PRINT "Bits.RotateRight(2, 1): "; Zanna.Math.Bits.RotateRight(2, 1)
PRINT "Bits.RotateRight(16, 4): "; Zanna.Math.Bits.RotateRight(16, 4)

' --- Swap ---
PRINT "--- Swap ---"
PRINT "Bits.Swap(1): "; Zanna.Math.Bits.Swap(1)
PRINT "Bits.Swap(256): "; Zanna.Math.Bits.Swap(256)

PRINT "=== Bits Audit Complete ==="
END
