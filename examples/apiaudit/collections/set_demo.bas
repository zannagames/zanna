' set_demo.bas - Comprehensive API audit for Zanna.Collections.Set
' Tests: New, Add, Has, Remove, Len, IsEmpty, Items, Clear,
'        Common, Diff, Merge, IsSubset, IsSuperset, IsDisjoint

PRINT "=== Set API Audit ==="

' --- New ---
PRINT "--- New ---"
DIM s AS OBJECT
s = Zanna.Collections.Set.New()
PRINT s.Count       ' 0
PRINT s.IsEmpty   ' 1

' --- Add ---
PRINT "--- Add ---"
DIM a AS OBJECT = Zanna.Core.Box.I64(10)
DIM b AS OBJECT = Zanna.Core.Box.I64(20)
DIM c AS OBJECT = Zanna.Core.Box.I64(30)
PRINT s.Add(a)    ' 1 (new)
PRINT s.Add(b)    ' 1 (new)
PRINT s.Add(c)    ' 1 (new)
PRINT s.Add(a)    ' 0 (duplicate)
PRINT s.Count       ' 3
PRINT s.IsEmpty   ' 0

' --- Has ---
PRINT "--- Has ---"
PRINT s.Has(a)    ' 1
PRINT s.Has(b)    ' 1
DIM d AS OBJECT = Zanna.Core.Box.I64(40)
PRINT s.Has(d)    ' 0

' --- Remove ---
PRINT "--- Remove ---"
PRINT s.Remove(b)  ' 1
PRINT s.Has(b)     ' 0
PRINT s.Count        ' 2
PRINT s.Remove(b)  ' 0 (already removed)

' --- Items ---
PRINT "--- Items ---"
DIM items AS Zanna.Collections.Seq
items = s.ToSeq()
PRINT items.Count    ' 2

' --- Merge (union) ---
PRINT "--- Merge ---"
DIM s1 AS OBJECT = Zanna.Collections.Set.New()
DIM x AS OBJECT = Zanna.Core.Box.Str("x")
DIM y AS OBJECT = Zanna.Core.Box.Str("y")
DIM z AS OBJECT = Zanna.Core.Box.Str("z")
s1.Add(x)
s1.Add(y)
s1.Add(z)

DIM s2 AS OBJECT = Zanna.Collections.Set.New()
DIM w AS OBJECT = Zanna.Core.Box.Str("w")
s2.Add(y)
s2.Add(z)
s2.Add(w)

DIM merged AS OBJECT = s1.Union(s2)
PRINT merged.Count   ' 4

' --- Intersect ---
PRINT "--- Intersect ---"
DIM common AS OBJECT = s1.Intersect(s2)
PRINT common.Count   ' 2

' --- Diff ---
PRINT "--- Diff ---"
DIM diff AS OBJECT = s1.Difference(s2)
PRINT diff.Count     ' 1

' --- IsSubset ---
PRINT "--- IsSubset ---"
DIM s3 AS OBJECT = Zanna.Collections.Set.New()
s3.Add(y)
s3.Add(z)
PRINT s3.IsSubset(s1)    ' 1

' --- IsSuperset ---
PRINT "--- IsSuperset ---"
PRINT s1.IsSuperset(s3)  ' 1

' --- IsDisjoint ---
PRINT "--- IsDisjoint ---"
DIM disj AS OBJECT = Zanna.Collections.Set.New()
disj.Add(w)
PRINT s1.IsDisjoint(disj) ' 1
PRINT s1.IsDisjoint(s2)   ' 0

' --- Clear ---
PRINT "--- Clear ---"
s1.Clear()
PRINT s1.Count       ' 0
PRINT s1.IsEmpty   ' 1

PRINT "=== Set audit complete ==="
END
