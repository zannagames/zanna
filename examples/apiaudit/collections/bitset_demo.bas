' bitset_demo.bas - Comprehensive API audit for Zanna.Collections.BitSet
' Tests: New, SetBit, GetBit, ClearBit, ToggleBit, Clear, SetAll, Count, Len,
'        IsEmpty, Not, And, Or, Xor, ToString

PRINT "=== BitSet API Audit ==="

' --- New ---
PRINT "--- New ---"
DIM bs AS OBJECT
bs = Zanna.Collections.BitSet.New(8)
PRINT bs.Length        ' 8
PRINT bs.Count      ' 0
PRINT bs.IsEmpty    ' 1

' --- Set / Get ---
PRINT "--- Set / Get ---"
bs.SetBit(0)
bs.SetBit(2)
bs.SetBit(4)
bs.SetBit(7)
PRINT bs.GetBit(0)     ' 1
PRINT bs.GetBit(1)     ' 0
PRINT bs.GetBit(2)     ' 1
PRINT bs.GetBit(4)     ' 1
PRINT bs.GetBit(7)     ' 1
PRINT bs.Count      ' 4
PRINT bs.IsEmpty    ' 0

' --- Clear (single bit) ---
PRINT "--- Clear ---"
bs.ClearBit(2)
PRINT bs.GetBit(2)     ' 0
PRINT bs.Count      ' 3

' --- Toggle ---
PRINT "--- Toggle ---"
bs.ToggleBit(1)
PRINT bs.GetBit(1)     ' 1
bs.ToggleBit(1)
PRINT bs.GetBit(1)     ' 0

' --- SetAll ---
PRINT "--- SetAll ---"
bs.SetAll()
PRINT bs.Count      ' 8
PRINT bs.GetBit(3)     ' 1

' --- ClearAll ---
PRINT "--- ClearAll ---"
bs.Clear()
PRINT bs.Count      ' 0
PRINT bs.IsEmpty    ' 1

' --- ToString ---
PRINT "--- ToString ---"
bs.SetBit(0)
bs.SetBit(3)
bs.SetBit(7)
PRINT bs.ToString()

' --- And ---
PRINT "--- And ---"
DIM a AS OBJECT = Zanna.Collections.BitSet.New(8)
a.SetBit(0)
a.SetBit(1)
a.SetBit(2)

DIM b AS OBJECT = Zanna.Collections.BitSet.New(8)
b.SetBit(1)
b.SetBit(2)
b.SetBit(3)

DIM andResult AS OBJECT = a.And(b)
PRINT andResult.GetBit(0)  ' 0
PRINT andResult.GetBit(1)  ' 1
PRINT andResult.GetBit(2)  ' 1
PRINT andResult.GetBit(3)  ' 0
PRINT andResult.Count   ' 2

' --- Or ---
PRINT "--- Or ---"
DIM orResult AS OBJECT = a.Or(b)
PRINT orResult.GetBit(0)   ' 1
PRINT orResult.GetBit(1)   ' 1
PRINT orResult.GetBit(2)   ' 1
PRINT orResult.GetBit(3)   ' 1
PRINT orResult.Count    ' 4

' --- Xor ---
PRINT "--- Xor ---"
DIM xorResult AS OBJECT = a.Xor(b)
PRINT xorResult.GetBit(0)  ' 1
PRINT xorResult.GetBit(1)  ' 0
PRINT xorResult.GetBit(2)  ' 0
PRINT xorResult.GetBit(3)  ' 1
PRINT xorResult.Count   ' 2

' --- Not ---
PRINT "--- Not ---"
DIM c AS OBJECT = Zanna.Collections.BitSet.New(4)
c.SetBit(0)
c.SetBit(2)
DIM notResult AS OBJECT = c.Not()
PRINT notResult.GetBit(0)  ' 0
PRINT notResult.GetBit(1)  ' 1
PRINT notResult.GetBit(2)  ' 0
PRINT notResult.GetBit(3)  ' 1

PRINT "=== BitSet audit complete ==="
END
