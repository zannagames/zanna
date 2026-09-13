' ring_demo.bas - Comprehensive API audit for Zanna.Collections.Ring
' Tests: New, NewDefault, Push, Pop, Peek, Get, Len, Capacity, IsEmpty, IsFull, Clear

PRINT "=== Ring API Audit ==="

' --- New ---
PRINT "--- New ---"
DIM r AS OBJECT
r = Zanna.Collections.Ring.New(3)
PRINT r.Count       ' 0
PRINT r.Capacity  ' 3
PRINT r.IsEmpty   ' 1
PRINT r.IsFull    ' 0

' --- Push / Len / IsFull ---
PRINT "--- Push / Len ---"
r.Push("a")
r.Push("b")
r.Push("c")
PRINT r.Count       ' 3
PRINT r.IsFull    ' 1
PRINT r.IsEmpty   ' 0

' --- Peek (oldest element) ---
PRINT "--- Peek ---"
PRINT r.Peek()    ' a

' --- Get (by logical index, 0 = oldest) ---
PRINT "--- Get ---"
PRINT r.Get(0)    ' a
PRINT r.Get(1)    ' b
PRINT r.Get(2)    ' c

' --- Push overflow (overwrites oldest) ---
PRINT "--- Push overflow ---"
r.Push("d")
PRINT r.Count       ' 3 (still at capacity)
PRINT r.Peek()    ' b (a was overwritten)
PRINT r.Get(0)    ' b
PRINT r.Get(1)    ' c
PRINT r.Get(2)    ' d

' --- Pop (removes oldest, FIFO) ---
PRINT "--- Pop ---"
PRINT r.Pop()     ' b
PRINT r.Count       ' 2
PRINT r.Pop()     ' c
PRINT r.Count       ' 1
PRINT r.IsFull    ' 0

' --- Push after pop ---
PRINT "--- Push after pop ---"
r.Push("e")
r.Push("f")
PRINT r.Count       ' 3
PRINT r.IsFull    ' 1
PRINT r.Get(0)    ' d
PRINT r.Get(1)    ' e
PRINT r.Get(2)    ' f

' --- Clear ---
PRINT "--- Clear ---"
r.Clear()
PRINT r.Count       ' 0
PRINT r.IsEmpty   ' 1
PRINT r.Capacity  ' 3 (capacity unchanged)

' --- NewDefault (default capacity) ---
PRINT "--- NewDefault ---"
DIM rd AS OBJECT
rd = Zanna.Collections.Ring.NewDefault()
PRINT rd.IsEmpty   ' 1
rd.Push(Zanna.Core.Box.I64(1))
PRINT rd.Count       ' 1

PRINT "=== Ring audit complete ==="
END
