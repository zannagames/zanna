' frozenset_demo.bas - Comprehensive API audit for Zanna.Collections.FrozenSet
' Tests: Empty, FromSeq, Len, IsEmpty, Has, Items, Union, Intersect,
'        Diff, IsSubset, Equals

PRINT "=== FrozenSet API Audit ==="

' --- Empty ---
PRINT "--- Empty ---"
DIM empty AS OBJECT
empty = Zanna.Collections.FrozenSet.Empty()
PRINT empty.Count       ' 0
PRINT empty.IsEmpty   ' 1

' --- FromSeq ---
PRINT "--- FromSeq ---"
DIM items AS Zanna.Collections.Seq
items = Zanna.Collections.Seq.New()
items.Push("apple")
items.Push("banana")
items.Push("cherry")
items.Push("apple")  ' duplicate
DIM fs AS OBJECT
fs = Zanna.Collections.FrozenSet.FromSeq(items)
PRINT fs.Count          ' 3
PRINT fs.IsEmpty      ' 0

' --- Has ---
PRINT "--- Has ---"
PRINT fs.Has("apple")    ' 1
PRINT fs.Has("banana")   ' 1
PRINT fs.Has("grape")    ' 0

' --- Items ---
PRINT "--- Items ---"
DIM all AS OBJECT
all = fs.ToSeq()
PRINT all.Count             ' 3

' --- Union ---
PRINT "--- Union ---"
DIM items2 AS Zanna.Collections.Seq
items2 = Zanna.Collections.Seq.New()
items2.Push("cherry")
items2.Push("date")
items2.Push("elderberry")
DIM fs2 AS OBJECT
fs2 = Zanna.Collections.FrozenSet.FromSeq(items2)
DIM united AS OBJECT
united = fs.Union(fs2)
PRINT united.Count          ' 5

' --- Intersect ---
PRINT "--- Intersect ---"
DIM inter AS OBJECT
inter = fs.Intersect(fs2)
PRINT inter.Count           ' 1
PRINT inter.Has("cherry") ' 1

' --- Diff ---
PRINT "--- Diff ---"
DIM diff AS OBJECT
diff = fs.Difference(fs2)
PRINT diff.Count            ' 2
PRINT diff.Has("apple")   ' 1
PRINT diff.Has("cherry")  ' 0

' --- IsSubset ---
PRINT "--- IsSubset ---"
DIM subItems AS Zanna.Collections.Seq
subItems = Zanna.Collections.Seq.New()
subItems.Push("apple")
subItems.Push("banana")
DIM subset AS OBJECT
subset = Zanna.Collections.FrozenSet.FromSeq(subItems)
PRINT subset.IsSubset(fs)    ' 1
PRINT fs.IsSubset(subset)    ' 0

' --- Equals ---
PRINT "--- Equals ---"
DIM items3 AS Zanna.Collections.Seq
items3 = Zanna.Collections.Seq.New()
items3.Push("banana")
items3.Push("cherry")
items3.Push("apple")
DIM fs3 AS OBJECT
fs3 = Zanna.Collections.FrozenSet.FromSeq(items3)
PRINT fs.Equals(fs3)      ' 1
PRINT fs.Equals(fs2)      ' 0

' --- Empty equals empty ---
PRINT "--- Empty equals empty ---"
DIM e1 AS OBJECT = Zanna.Collections.FrozenSet.Empty()
DIM e2 AS OBJECT = Zanna.Collections.FrozenSet.Empty()
PRINT e1.Equals(e2)       ' 1

PRINT "=== FrozenSet audit complete ==="
END
