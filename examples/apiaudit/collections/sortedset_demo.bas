' sortedset_demo.bas - Comprehensive API audit for Zanna.Collections.SortedSet
' Tests: New, Add, Remove, Has, First, Last, Floor, Ceiling, Lower, Higher,
'        At, IndexOf, ToSeq, Range, Take, Skip, Union, Intersect, Difference,
'        IsSubset, Len, IsEmpty, Clear

PRINT "=== SortedSet API Audit ==="

' --- New ---
PRINT "--- New ---"
DIM ss AS OBJECT
ss = Zanna.Collections.SortedSet.New()
PRINT ss.Count       ' 0
PRINT ss.IsEmpty   ' 1

' --- Add ---
PRINT "--- Add ---"
PRINT ss.Add("cherry")     ' 1 (new)
PRINT ss.Add("apple")      ' 1 (new)
PRINT ss.Add("banana")     ' 1 (new)
PRINT ss.Add("date")       ' 1 (new)
PRINT ss.Add("elderberry") ' 1 (new)
PRINT ss.Add("apple")      ' 0 (duplicate)
PRINT ss.Count               ' 5
PRINT ss.IsEmpty           ' 0

' --- Has ---
PRINT "--- Has ---"
PRINT ss.Has("apple")    ' 1
PRINT ss.Has("banana")   ' 1
PRINT ss.Has("fig")      ' 0

' --- First / Last (sorted order) ---
PRINT "--- First / Last ---"
PRINT ss.First()   ' apple
PRINT ss.Last()    ' elderberry

' --- At (index access, sorted) ---
PRINT "--- At ---"
PRINT ss.At(0)  ' apple
PRINT ss.At(1)  ' banana
PRINT ss.At(2)  ' cherry
PRINT ss.At(3)  ' date
PRINT ss.At(4)  ' elderberry

' --- IndexOf ---
PRINT "--- IndexOf ---"
PRINT ss.IndexOf("apple")      ' 0
PRINT ss.IndexOf("cherry")     ' 2
PRINT ss.IndexOf("elderberry") ' 4

' --- Floor (greatest element <= key) ---
PRINT "--- Floor ---"
PRINT ss.Floor("cherry")  ' cherry
PRINT ss.Floor("c")       ' banana

' --- Ceiling (smallest element >= key) ---
PRINT "--- Ceiling ---"
PRINT ss.Ceiling("cherry")   ' cherry
PRINT ss.Ceiling("c")        ' cherry

' --- Lower (greatest element < key) ---
PRINT "--- Lower ---"
PRINT ss.Lower("cherry")  ' banana

' --- Higher (smallest element > key) ---
PRINT "--- Higher ---"
PRINT ss.Higher("cherry") ' date

' --- ToSeq ---
PRINT "--- ToSeq ---"
DIM items AS OBJECT
items = ss.ToSeq()
PRINT items.Count  ' 5

' --- Range ---
PRINT "--- Range ---"
DIM ranged AS OBJECT
ranged = ss.Range("banana", "date")
PRINT ranged.Count  ' 3

' --- Take ---
PRINT "--- Take ---"
DIM taken AS OBJECT
taken = ss.Take(3)
PRINT taken.Count  ' 3

' --- Skip ---
PRINT "--- Skip ---"
DIM skipped AS OBJECT
skipped = ss.Skip(2)
PRINT skipped.Count  ' 3

' --- Remove ---
PRINT "--- Remove ---"
PRINT ss.Remove("banana")   ' 1
PRINT ss.Has("banana")    ' 0
PRINT ss.Count              ' 4
PRINT ss.Remove("banana")   ' 0

' --- Union ---
PRINT "--- Union ---"
DIM ss2 AS OBJECT
ss2 = Zanna.Collections.SortedSet.New()
ss2.Add("apple")
ss2.Add("fig")
ss2.Add("grape")
DIM merged AS OBJECT
merged = ss.Union(ss2)
PRINT merged.Count  ' 6

' --- Intersect ---
PRINT "--- Intersect ---"
DIM common AS OBJECT
common = ss.Intersect(ss2)
PRINT common.Count  ' 1

' --- Difference ---
PRINT "--- Difference ---"
DIM diff AS OBJECT
diff = ss.Difference(ss2)
PRINT diff.Count  ' 3

' --- IsSubset ---
PRINT "--- IsSubset ---"
DIM sub1 AS OBJECT
sub1 = Zanna.Collections.SortedSet.New()
sub1.Add("apple")
sub1.Add("cherry")
PRINT sub1.IsSubset(ss)  ' 1

' --- Clear ---
PRINT "--- Clear ---"
ss.Clear()
PRINT ss.Count       ' 0
PRINT ss.IsEmpty   ' 1

PRINT "=== SortedSet audit complete ==="
END
