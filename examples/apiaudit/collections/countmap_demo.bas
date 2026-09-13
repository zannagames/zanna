' countmap_demo.bas - Comprehensive API audit for Zanna.Collections.CountMap
' Tests: New, Increment, IncrementBy, Decrement, Get, Set, Has, Total, Keys,
'        MostCommon, Remove, Clear, Len, IsEmpty

PRINT "=== CountMap API Audit ==="

' --- New ---
PRINT "--- New ---"
DIM cm AS OBJECT
cm = Zanna.Collections.CountMap.New()
PRINT cm.Count       ' 0
PRINT cm.IsEmpty   ' 1

' --- Increment / Count ---
PRINT "--- Increment / Count ---"
PRINT cm.Increment("apple")     ' 1
PRINT cm.Increment("apple")     ' 2
PRINT cm.Increment("banana")    ' 1
PRINT cm.Increment("cherry")    ' 1
PRINT cm.Increment("cherry")    ' 2
PRINT cm.Increment("cherry")    ' 3
PRINT cm.Count               ' 3
PRINT cm.IsEmpty           ' 0

' --- IncrementBy ---
PRINT "--- IncrementBy ---"
PRINT cm.IncrementBy("banana", 5)  ' 6

' --- Get ---
PRINT "--- Get ---"
PRINT cm.Get("apple")    ' 2
PRINT cm.Get("banana")   ' 6
PRINT cm.Get("cherry")   ' 3
PRINT cm.Get("grape")    ' 0

' --- Has ---
PRINT "--- Has ---"
PRINT cm.Has("apple")    ' 1
PRINT cm.Has("grape")    ' 0

' --- Set ---
PRINT "--- Set ---"
cm.Set("date", 10)
PRINT cm.Get("date")     ' 10
PRINT cm.Count              ' 4

' --- Total ---
PRINT "--- Total ---"
PRINT cm.Total            ' 21

' --- Decrement ---
PRINT "--- Decrement ---"
PRINT cm.Decrement("apple")    ' 1
PRINT cm.Decrement("apple")    ' 0 (removed)
PRINT cm.Has("apple")    ' 0
PRINT cm.Count              ' 3

' --- Keys ---
PRINT "--- Keys ---"
DIM keys AS OBJECT
keys = cm.Keys()
PRINT keys.Count            ' 3

' --- MostCommon ---
PRINT "--- MostCommon ---"
DIM top AS OBJECT
top = cm.MostCommon(2)
PRINT top.Count             ' 2

' --- Remove ---
PRINT "--- Remove ---"
PRINT cm.Remove("date")    ' 1
PRINT cm.Has("date")       ' 0
PRINT cm.Count                ' 2
PRINT cm.Remove("date")    ' 0

' --- Clear ---
PRINT "--- Clear ---"
cm.Clear()
PRINT cm.Count                ' 0
PRINT cm.IsEmpty            ' 1

PRINT "=== CountMap audit complete ==="
END
