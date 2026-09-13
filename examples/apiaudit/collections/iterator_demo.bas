' iterator_demo.bas - Comprehensive API audit for Zanna.Collections.Iterator
' Tests: FromSeq, HasNext, Next, Peek, Reset, Index, Count, ToSeq, Skip

PRINT "=== Iterator API Audit ==="

' --- FromSeq ---
PRINT "--- FromSeq ---"
DIM seq AS Zanna.Collections.Seq
seq = Zanna.Collections.Seq.New()
seq.Push("a")
seq.Push("b")
seq.Push("c")
seq.Push("d")
seq.Push("e")

DIM it AS OBJECT
it = Zanna.Collections.Iterator.FromSeq(seq)
PRINT it.Count       ' 5

' --- HasNext ---
PRINT "--- HasNext ---"
PRINT it.HasNext()     ' 1

' --- Index ---
PRINT "--- Index ---"
PRINT it.Index       ' 0

' --- Peek ---
PRINT "--- Peek ---"
PRINT it.Peek()      ' a
PRINT it.Peek()      ' a (still a)
PRINT it.Index       ' 0

' --- Next ---
PRINT "--- Next ---"
PRINT it.Next()      ' a
PRINT it.Index       ' 1
PRINT it.Next()      ' b
PRINT it.Index       ' 2

' --- Peek after Next ---
PRINT "--- Peek after Next ---"
PRINT it.Peek()      ' c

' --- Skip ---
PRINT "--- Skip ---"
PRINT it.Skip(2)     ' 2 (skipped c, d)
PRINT it.Index       ' 4
PRINT it.Peek()      ' e

' --- Next to exhaustion ---
PRINT "--- Next to exhaustion ---"
PRINT it.Next()      ' e
PRINT it.HasNext()     ' 0
PRINT it.Index       ' 5

' --- Reset ---
PRINT "--- Reset ---"
it.Reset()
PRINT it.Index       ' 0
PRINT it.HasNext()     ' 1
PRINT it.Peek()      ' a

' --- ToSeq ---
PRINT "--- ToSeq ---"
it.Next()   ' consume a
it.Next()   ' consume b
DIM rest AS OBJECT
rest = it.ToSeq()
PRINT rest.Count       ' 3
PRINT rest.Get(0)    ' c
PRINT rest.Get(1)    ' d
PRINT rest.Get(2)    ' e

' --- Skip past end ---
PRINT "--- Skip past end ---"
DIM it2 AS OBJECT
it2 = Zanna.Collections.Iterator.FromSeq(seq)
PRINT it2.Skip(10)   ' 5
PRINT it2.HasNext()    ' 0

' --- Empty iterator ---
PRINT "--- Empty iterator ---"
DIM emptySeq AS Zanna.Collections.Seq
emptySeq = Zanna.Collections.Seq.New()
DIM it3 AS OBJECT
it3 = Zanna.Collections.Iterator.FromSeq(emptySeq)
PRINT it3.HasNext()    ' 0
PRINT it3.Count      ' 0
PRINT it3.Index      ' 0

PRINT "=== Iterator audit complete ==="
END
