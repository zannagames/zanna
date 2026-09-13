' bag_demo.bas - Comprehensive API audit for Zanna.Collections.StringSet
' Tests: New, Add, Has, Remove, Count, IsEmpty, ToSeq, Clear, Union, Intersect, Difference

PRINT "=== StringSet API Audit ==="

' --- New ---
PRINT "--- New ---"
DIM bag AS OBJECT
bag = Zanna.Collections.StringSet.New()
PRINT bag.Count       ' 0
PRINT bag.IsEmpty   ' 1

' --- Add / Count ---
PRINT "--- Add / Count ---"
PRINT bag.Add("apple")    ' 1 (new)
PRINT bag.Add("banana")   ' 1 (new)
PRINT bag.Add("cherry")   ' 1 (new)
PRINT bag.Add("apple")    ' 0 (duplicate)
PRINT bag.Count              ' 3
PRINT bag.IsEmpty          ' 0

' --- Has ---
PRINT "--- Has ---"
PRINT bag.Has("apple")    ' 1
PRINT bag.Has("banana")   ' 1
PRINT bag.Has("grape")    ' 0

' --- Remove ---
PRINT "--- Remove ---"
PRINT bag.Remove("banana")  ' 1
PRINT bag.Has("banana")   ' 0
PRINT bag.Count              ' 2
PRINT bag.Remove("banana")  ' 0

' --- ToSeq ---
PRINT "--- ToSeq ---"
DIM items AS OBJECT
items = bag.ToSeq()
PRINT items.Count            ' 2

' --- Union ---
PRINT "--- Union ---"
DIM b1 AS OBJECT = Zanna.Collections.StringSet.New()
b1.Add("a")
b1.Add("b")
b1.Add("c")

DIM b2 AS OBJECT = Zanna.Collections.StringSet.New()
b2.Add("b")
b2.Add("c")
b2.Add("d")

DIM merged AS OBJECT = b1.Union(b2)
PRINT merged.Count           ' 4
PRINT merged.Has("a")      ' 1
PRINT merged.Has("d")      ' 1

' --- Intersect ---
PRINT "--- Intersect ---"
DIM common AS OBJECT = b1.Intersect(b2)
PRINT common.Count           ' 2
PRINT common.Has("b")      ' 1
PRINT common.Has("c")      ' 1
PRINT common.Has("a")      ' 0

' --- Difference ---
PRINT "--- Difference ---"
DIM diff AS OBJECT = b1.Difference(b2)
PRINT diff.Count             ' 1
PRINT diff.Has("a")        ' 1
PRINT diff.Has("b")        ' 0

' --- Clear ---
PRINT "--- Clear ---"
b1.Clear()
PRINT b1.Count               ' 0
PRINT b1.IsEmpty           ' 1

PRINT "=== StringSet audit complete ==="
END
