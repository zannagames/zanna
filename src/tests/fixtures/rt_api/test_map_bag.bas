' test_map_bag.bas — Map, Bag, CountMap, OrderedMap
DIM m AS Zanna.Collections.Map
m = Zanna.Collections.Map.New()
PRINT "map empty: "; m.IsEmpty
m.Set("a", "1")
m.Set("b", "2")
m.Set("c", "3")
PRINT "map len: "; m.Count
PRINT "has a: "; m.Has("a")
PRINT "has z: "; m.Has("z")
m.SetIfMissing("a", "99")
m.Remove("b")
PRINT "map len after remove: "; m.Count
m.Clear()
PRINT "map empty after clear: "; m.IsEmpty

DIM b AS Zanna.Collections.StringSet
b = Zanna.Collections.StringSet.New()
PRINT "bag empty: "; b.IsEmpty
b.Add("x")
b.Add("y")
b.Add("z")
b.Add("x")
PRINT "bag len: "; b.Count
PRINT "bag has x: "; b.Has("x")
PRINT "bag has w: "; b.Has("w")
b.Remove("x")
PRINT "bag len after remove: "; b.Count
b.Clear()
PRINT "bag empty after clear: "; b.IsEmpty

DIM cm AS Zanna.Collections.CountMap
cm = Zanna.Collections.CountMap.New()
cm.Increment("a")
cm.Increment("a")
cm.Increment("b")
cm.IncrementBy("c", 5)
PRINT "cm get a: "; cm.Get("a")
PRINT "cm get c: "; cm.Get("c")
PRINT "cm total: "; cm.Total
PRINT "cm len: "; cm.Count
cm.Decrement("a")
PRINT "cm get a after dec: "; cm.Get("a")
cm.Set("d", 10)
PRINT "cm get d: "; cm.Get("d")
PRINT "cm has d: "; cm.Has("d")
cm.Remove("d")
PRINT "cm has d after remove: "; cm.Has("d")

DIM om AS Zanna.Collections.OrderedMap
om = Zanna.Collections.OrderedMap.New()
om.Set("first", "1")
om.Set("second", "2")
om.Set("third", "3")
PRINT "om len: "; om.Count
PRINT "om keyat 0: "; om.KeyAt(0)
PRINT "om has first: "; om.Has("first")
om.Remove("second")
PRINT "om len after remove: "; om.Count

PRINT "done"
END
