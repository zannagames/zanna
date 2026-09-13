' seq_demo.bas - Comprehensive API audit for Zanna.Collections.Seq
' Tests: New, WithCapacity, Push, Pop, Get, Set, Len, Capacity, IsEmpty, Find, FindOption, Has,
'        Insert, Remove, Clear, Clone, First, Last, Peek, Slice, Reverse,
'        Sort, SortDesc, Shuffle, Take, Drop, PushAll
' Note: Inline Zanna.Core.Box.ToI64(s.Get(N)) can corrupt heap state for
'       subsequent calls. Store Get results in a variable first.

PRINT "=== Seq API Audit ==="

' --- New ---
PRINT "--- New ---"
DIM s AS Zanna.Collections.Seq
s = Zanna.Collections.Seq.New()
PRINT s.Count       ' 0
PRINT s.IsEmpty   ' 1

' --- WithCapacity ---
PRINT "--- WithCapacity ---"
DIM sc AS Zanna.Collections.Seq
sc = Zanna.Collections.Seq.WithCapacity(10)
PRINT sc.Count      ' 0
PRINT sc.Capacity ' 10

' --- Push / Len / Capacity ---
PRINT "--- Push / Len / Capacity ---"
s.Push(Zanna.Core.Box.I64(10))
s.Push(Zanna.Core.Box.I64(20))
s.Push(Zanna.Core.Box.I64(30))
PRINT s.Count       ' 3
PRINT s.IsEmpty   ' 0

' --- Get ---
PRINT "--- Get ---"
DIM v0 AS OBJECT
v0 = s.Get(0)
PRINT Zanna.Core.Box.ToI64(v0)  ' 10
DIM v1 AS OBJECT
v1 = s.Get(1)
PRINT Zanna.Core.Box.ToI64(v1)  ' 20
DIM v2 AS OBJECT
v2 = s.Get(2)
PRINT Zanna.Core.Box.ToI64(v2)  ' 30

' --- Set ---
PRINT "--- Set ---"
s.Set(1, Zanna.Core.Box.I64(25))
DIM sv AS OBJECT
sv = s.Get(1)
PRINT Zanna.Core.Box.ToI64(sv)  ' 25

' --- First / Last / Peek ---
PRINT "--- First / Last / Peek ---"
DIM fv AS OBJECT
fv = s.First()
PRINT Zanna.Core.Box.ToI64(fv)  ' 10
DIM lv AS OBJECT
lv = s.Last()
PRINT Zanna.Core.Box.ToI64(lv)  ' 30
DIM pk AS OBJECT
pk = s.Peek()
PRINT Zanna.Core.Box.ToI64(pk)  ' 30

' --- Find / Has ---
PRINT "--- Find / Has ---"
PRINT s.FindOption(Zanna.Core.Box.I64(10)).UnwrapOrI64(-1)   ' 0
PRINT s.FindOption(Zanna.Core.Box.I64(99)).UnwrapOrI64(-1)   ' -1
PRINT s.Has(Zanna.Core.Box.I64(30))    ' 1
PRINT s.Has(Zanna.Core.Box.I64(99))    ' 0
DIM found AS OBJECT
found = s.FindOption(Zanna.Core.Box.I64(10))
PRINT found.IsSome
PRINT found.UnwrapI64()                ' 0
PRINT s.FindOption(Zanna.Core.Box.I64(99)).IsNone

' --- Insert ---
PRINT "--- Insert ---"
s.Insert(1, Zanna.Core.Box.I64(15))
PRINT s.Count                             ' 4
DIM iv1 AS OBJECT
iv1 = s.Get(1)
PRINT Zanna.Core.Box.ToI64(iv1)        ' 15
DIM iv2 AS OBJECT
iv2 = s.Get(2)
PRINT Zanna.Core.Box.ToI64(iv2)        ' 25

' --- Remove ---
PRINT "--- Remove ---"
DIM removed AS OBJECT
removed = s.RemoveAt(1)
PRINT Zanna.Core.Box.ToI64(removed)    ' 15
PRINT s.Count                             ' 3

' --- Pop ---
PRINT "--- Pop ---"
DIM popped AS OBJECT
popped = s.Pop()
PRINT Zanna.Core.Box.ToI64(popped)     ' 30
PRINT s.Count                             ' 2

' --- Clone ---
PRINT "--- Clone ---"
DIM c AS Zanna.Collections.Seq
c = s.Clone()
PRINT c.Count                             ' 2
DIM cv0 AS OBJECT
cv0 = c.Get(0)
PRINT Zanna.Core.Box.ToI64(cv0)        ' 10
DIM cv1 AS OBJECT
cv1 = c.Get(1)
PRINT Zanna.Core.Box.ToI64(cv1)        ' 25

' --- Slice ---
PRINT "--- Slice ---"
s.Push(Zanna.Core.Box.I64(30))
s.Push(Zanna.Core.Box.I64(40))
DIM sl AS Zanna.Collections.Seq
sl = s.Slice(1, 3)
PRINT sl.Count                            ' 2
DIM slv0 AS OBJECT
slv0 = sl.Get(0)
PRINT Zanna.Core.Box.ToI64(slv0)       ' 25
DIM slv1 AS OBJECT
slv1 = sl.Get(1)
PRINT Zanna.Core.Box.ToI64(slv1)       ' 30

' --- Reverse ---
PRINT "--- Reverse ---"
s.Reverse()
DIM rv0 AS OBJECT
rv0 = s.Get(0)
PRINT Zanna.Core.Box.ToI64(rv0)        ' 40
DIM rv3 AS OBJECT
rv3 = s.Get(3)
PRINT Zanna.Core.Box.ToI64(rv3)        ' 10

' --- Sort ---
PRINT "--- Sort ---"
s.Sort()
DIM so0 AS OBJECT
so0 = s.Get(0)
PRINT Zanna.Core.Box.ToI64(so0)        ' 10
DIM so3 AS OBJECT
so3 = s.Get(3)
PRINT Zanna.Core.Box.ToI64(so3)        ' 40

' --- SortDesc ---
PRINT "--- SortDesc ---"
s.SortDesc()
DIM sd0 AS OBJECT
sd0 = s.Get(0)
PRINT Zanna.Core.Box.ToI64(sd0)        ' 40
DIM sd3 AS OBJECT
sd3 = s.Get(3)
PRINT Zanna.Core.Box.ToI64(sd3)        ' 10

' --- Shuffle ---
PRINT "--- Shuffle ---"
s.Shuffle()
PRINT s.Count                             ' 4

' --- Take ---
PRINT "--- Take ---"
s.Sort()
DIM t AS Zanna.Collections.Seq
t = s.Take(2)
PRINT t.Count                             ' 2
DIM tv0 AS OBJECT
tv0 = t.Get(0)
PRINT Zanna.Core.Box.ToI64(tv0)        ' 10

' --- Drop ---
PRINT "--- Drop ---"
DIM d AS Zanna.Collections.Seq
d = s.Drop(2)
PRINT d.Count                             ' 2
DIM dv0 AS OBJECT
dv0 = d.Get(0)
PRINT Zanna.Core.Box.ToI64(dv0)        ' 30

' --- PushAll ---
PRINT "--- PushAll ---"
DIM a AS Zanna.Collections.Seq
a = Zanna.Collections.Seq.New()
a.Push(Zanna.Core.Box.I64(1))
a.Push(Zanna.Core.Box.I64(2))
DIM b AS Zanna.Collections.Seq
b = Zanna.Collections.Seq.New()
b.Push(Zanna.Core.Box.I64(3))
b.Push(Zanna.Core.Box.I64(4))
a.PushAll(b)
PRINT a.Count                             ' 4
DIM av2 AS OBJECT
av2 = a.Get(2)
PRINT Zanna.Core.Box.ToI64(av2)        ' 3
DIM av3 AS OBJECT
av3 = a.Get(3)
PRINT Zanna.Core.Box.ToI64(av3)        ' 4

' --- Clear ---
PRINT "--- Clear ---"
a.Clear()
PRINT a.Count                             ' 0
PRINT a.IsEmpty                         ' 1

PRINT "=== Seq audit complete ==="
END
