' treemap_demo.bas - Comprehensive API audit for Zanna.Collections.SortedMap
' Tests: New, Set, Get, Has, Drop, Clear, Len, IsEmpty,
'        Keys, Values, First, Last, Floor, Ceil

PRINT "=== TreeMap API Audit ==="

' --- New ---
PRINT "--- New ---"
DIM tm AS Zanna.Collections.SortedMap
tm = Zanna.Collections.SortedMap.New()
PRINT tm.Count       ' 0
PRINT tm.IsEmpty   ' 1

' --- Set / Len ---
PRINT "--- Set / Len ---"
tm.Set("cherry", "red")
tm.Set("apple", "green")
tm.Set("banana", "yellow")
tm.Set("date", "brown")
PRINT tm.Count       ' 4
PRINT tm.IsEmpty   ' 0

' --- Get ---
PRINT "--- Get ---"
PRINT tm.Get("apple")    ' green
PRINT tm.Get("cherry")   ' red
PRINT tm.Get("banana")   ' yellow

' --- Has ---
PRINT "--- Has ---"
PRINT tm.Has("apple")    ' 1
PRINT tm.Has("fig")      ' 0

' --- Set (update existing) ---
PRINT "--- Set (update) ---"
tm.Set("apple", "red")
PRINT tm.Get("apple")    ' red
PRINT tm.Count              ' 4

' --- First / Last ---
PRINT "--- First / Last ---"
PRINT tm.First()          ' apple
PRINT tm.Last()           ' date

' --- Floor / Ceil ---
PRINT "--- Floor / Ceil ---"
PRINT tm.Floor("cat")    ' banana
PRINT tm.Ceiling("cat")     ' cherry
PRINT tm.Floor("apple")  ' apple
PRINT tm.Ceiling("date")    ' date

' --- Keys (sorted order) ---
PRINT "--- Keys ---"
DIM keys AS OBJECT
keys = tm.Keys()
PRINT keys.Count            ' 4
PRINT keys.Get(0)         ' apple
PRINT keys.Get(1)         ' banana
PRINT keys.Get(2)         ' cherry
PRINT keys.Get(3)         ' date

' --- Values (key-sorted order) ---
PRINT "--- Values ---"
DIM vals AS OBJECT
vals = tm.Values()
PRINT vals.Count            ' 4

' --- Remove ---
PRINT "--- Remove ---"
PRINT tm.Remove("banana")  ' 1
PRINT tm.Has("banana")     ' 0
PRINT tm.Count                ' 3
PRINT tm.Remove("banana")  ' 0

' --- Clear ---
PRINT "--- Clear ---"
tm.Clear()
PRINT tm.Count              ' 0
PRINT tm.IsEmpty          ' 1

PRINT "=== TreeMap audit complete ==="
END
