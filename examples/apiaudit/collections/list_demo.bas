' list_demo.bas - Comprehensive API audit for Zanna.Collections.List
' Tests: New, Push, Pop, Get, Set, Len, IsEmpty, Find, FindOption, Has, Insert,
'        Remove, RemoveAt, Clear, Reverse, First, Last, Slice

PRINT "=== List API Audit ==="

' --- New ---
PRINT "--- New ---"
DIM list AS Zanna.Collections.List
list = Zanna.Collections.List.New()
PRINT list.Count       ' 0
PRINT list.IsEmpty   ' 1

' --- Push / Len ---
PRINT "--- Push / Len ---"
DIM a AS OBJECT = Zanna.Core.Box.Str("apple")
DIM b AS OBJECT = Zanna.Core.Box.Str("banana")
DIM c AS OBJECT = Zanna.Core.Box.Str("cherry")
list.Push(a)
list.Push(b)
list.Push(c)
PRINT list.Count       ' 3
PRINT list.IsEmpty   ' 0

' --- Get ---
PRINT "--- Get ---"
PRINT Zanna.Core.Box.ToStr(list.Get(0))  ' apple
PRINT Zanna.Core.Box.ToStr(list.Get(1))  ' banana
PRINT Zanna.Core.Box.ToStr(list.Get(2))  ' cherry

' --- Set ---
PRINT "--- Set ---"
DIM d AS OBJECT = Zanna.Core.Box.Str("date")
list.Set(1, d)
PRINT Zanna.Core.Box.ToStr(list.Get(1))  ' date

' --- First / Last ---
PRINT "--- First / Last ---"
PRINT Zanna.Core.Box.ToStr(list.First())  ' apple
PRINT Zanna.Core.Box.ToStr(list.Last())   ' cherry

' --- Find / Has ---
PRINT "--- Find / Has ---"
PRINT list.FindOption(a).UnwrapOrI64(-1)    ' 0
PRINT list.FindOption(b).UnwrapOrI64(-1)    ' -1 (was replaced)
PRINT list.Has(a)     ' 1
PRINT list.Has(b)     ' 0
DIM found AS OBJECT
found = list.FindOption(a)
PRINT found.IsSome
PRINT found.UnwrapI64()  ' 0
PRINT list.FindOption(b).IsNone

' --- Insert ---
PRINT "--- Insert ---"
DIM e AS OBJECT = Zanna.Core.Box.Str("elderberry")
list.Insert(1, e)
PRINT list.Count                            ' 4
PRINT Zanna.Core.Box.ToStr(list.Get(1))  ' elderberry
PRINT Zanna.Core.Box.ToStr(list.Get(2))  ' date

' --- Remove (by reference) ---
PRINT "--- Remove ---"
PRINT list.Remove(e)   ' 1
PRINT list.Count          ' 3
PRINT list.Remove(e)   ' 0 (already removed)

' --- RemoveAt ---
PRINT "--- RemoveAt ---"
list.RemoveAt(1)
PRINT list.Count                            ' 2
PRINT Zanna.Core.Box.ToStr(list.Get(0))  ' apple
PRINT Zanna.Core.Box.ToStr(list.Get(1))  ' cherry

' --- Slice ---
PRINT "--- Slice ---"
list.Push(Zanna.Core.Box.Str("fig"))
list.Push(Zanna.Core.Box.Str("grape"))
DIM sl AS Zanna.Collections.List
sl = list.Slice(1, 3)
PRINT sl.Count                              ' 2
PRINT Zanna.Core.Box.ToStr(sl.Get(0))    ' cherry
PRINT Zanna.Core.Box.ToStr(sl.Get(1))    ' fig

' --- Reverse ---
PRINT "--- Reverse ---"
list.Reverse()
PRINT Zanna.Core.Box.ToStr(list.Get(0))  ' grape
PRINT Zanna.Core.Box.ToStr(list.Get(3))  ' apple

' --- Pop ---
PRINT "--- Pop ---"
DIM popped AS OBJECT
popped = list.Pop()
PRINT Zanna.Core.Box.ToStr(popped)       ' apple
PRINT list.Count                            ' 3

' --- Clear ---
PRINT "--- Clear ---"
list.Clear()
PRINT list.Count       ' 0
PRINT list.IsEmpty   ' 1

PRINT "=== List audit complete ==="
END
