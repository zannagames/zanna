' trie_demo.bas - Comprehensive API audit for Zanna.Collections.Trie
' Tests: New, Put, Get, Has, HasPrefix, Remove, Clear, Len, IsEmpty,
'        Keys, LongestPrefix, WithPrefix

PRINT "=== Trie API Audit ==="

' --- New ---
PRINT "--- New ---"
DIM t AS OBJECT
t = Zanna.Collections.Trie.New()
PRINT t.Count       ' 0
PRINT t.IsEmpty   ' 1

' --- Put / Len ---
PRINT "--- Put / Len ---"
t.Set("cat", Zanna.Core.Box.I64(1))
t.Set("car", Zanna.Core.Box.I64(2))
t.Set("card", Zanna.Core.Box.I64(3))
t.Set("care", Zanna.Core.Box.I64(4))
t.Set("dog", Zanna.Core.Box.I64(5))
PRINT t.Count       ' 5
PRINT t.IsEmpty   ' 0

' --- Get ---
PRINT "--- Get ---"
PRINT Zanna.Core.Box.ToI64(t.Get("cat"))   ' 1
PRINT Zanna.Core.Box.ToI64(t.Get("car"))   ' 2
PRINT Zanna.Core.Box.ToI64(t.Get("card"))  ' 3
PRINT Zanna.Core.Box.ToI64(t.Get("care"))  ' 4

' --- Has ---
PRINT "--- Has ---"
PRINT t.Has("cat")     ' 1
PRINT t.Has("ca")      ' 0 (prefix but not a key)
PRINT t.Has("dogs")    ' 0

' --- HasPrefix ---
PRINT "--- HasPrefix ---"
PRINT t.HasPrefix("ca")   ' 1
PRINT t.HasPrefix("car")  ' 1
PRINT t.HasPrefix("do")   ' 1
PRINT t.HasPrefix("x")    ' 0

' --- LongestPrefix ---
PRINT "--- LongestPrefix ---"
PRINT t.LongestPrefix("card_game")  ' card
PRINT t.LongestPrefix("caring")     ' care
PRINT t.LongestPrefix("catalog")    ' cat
PRINT t.LongestPrefix("dogs")       ' dog

' --- WithPrefix ---
PRINT "--- WithPrefix ---"
DIM carKeys AS OBJECT
carKeys = t.WithPrefix("car")
PRINT carKeys.Count                    ' 3 (car, card, care)
DIM caKeys AS OBJECT
caKeys = t.WithPrefix("ca")
PRINT caKeys.Count                     ' 4 (cat, car, card, care)
DIM dKeys AS OBJECT
dKeys = t.WithPrefix("dog")
PRINT dKeys.Count                      ' 1 (dog)

' --- Keys ---
PRINT "--- Keys ---"
DIM keys AS OBJECT
keys = t.Keys()
PRINT keys.Count                       ' 5

' --- Put (update existing) ---
PRINT "--- Put (update) ---"
t.Set("cat", Zanna.Core.Box.I64(100))
PRINT Zanna.Core.Box.ToI64(t.Get("cat"))  ' 100
PRINT t.Count                                ' 5

' --- Remove ---
PRINT "--- Remove ---"
PRINT t.Remove("card")   ' 1
PRINT t.Has("card")      ' 0
PRINT t.Count              ' 4
PRINT t.Remove("card")   ' 0
PRINT t.Has("car")       ' 1
PRINT t.Has("care")      ' 1

' --- Clear ---
PRINT "--- Clear ---"
t.Clear()
PRINT t.Count              ' 0
PRINT t.IsEmpty          ' 1

PRINT "=== Trie audit complete ==="
END
