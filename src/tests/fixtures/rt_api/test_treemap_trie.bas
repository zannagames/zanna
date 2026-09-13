' test_treemap_trie.bas — TreeMap, Trie, SortedSet, BiMap
DIM tm AS Zanna.Collections.SortedMap
tm = Zanna.Collections.SortedMap.New()
PRINT "tm empty: "; tm.IsEmpty
tm.Set("banana", "yellow")
tm.Set("apple", "red")
tm.Set("cherry", "dark")
PRINT "tm len: "; tm.Count
PRINT "tm has banana: "; tm.Has("banana")
PRINT "tm first: "; tm.First()
PRINT "tm last: "; tm.Last()
PRINT "tm floor blueberry: "; tm.Floor("blueberry")
PRINT "tm ceil blueberry: "; tm.Ceiling("blueberry")
tm.Remove("apple")
PRINT "tm len after remove: "; tm.Count

DIM tr AS Zanna.Collections.Trie
tr = Zanna.Collections.Trie.New()
PRINT "tr empty: "; tr.IsEmpty
tr.Set("hello", "1")
tr.Set("help", "2")
tr.Set("world", "3")
PRINT "tr len: "; tr.Count
PRINT "tr has help: "; tr.Has("help")
PRINT "tr hasprefix hel: "; tr.HasPrefix("hel")
PRINT "tr hasprefix xyz: "; tr.HasPrefix("xyz")
PRINT "tr longestprefix helping: "; tr.LongestPrefix("helping")
tr.Remove("help")
PRINT "tr len after remove: "; tr.Count

DIM ss AS Zanna.Collections.SortedSet
ss = Zanna.Collections.SortedSet.New()
PRINT "ss empty: "; ss.IsEmpty
ss.Add("cherry")
ss.Add("apple")
ss.Add("banana")
PRINT "ss len: "; ss.Count
PRINT "ss first: "; ss.First()
PRINT "ss last: "; ss.Last()
PRINT "ss has apple: "; ss.Has("apple")
PRINT "ss at 1: "; ss.At(1)
PRINT "ss indexof banana: "; ss.IndexOf("banana")
PRINT "ss floor blueberry: "; ss.Floor("blueberry")
PRINT "ss ceil blueberry: "; ss.Ceiling("blueberry")
ss.Remove("apple")
PRINT "ss len after drop: "; ss.Count

DIM bm AS Zanna.Collections.BiMap
bm = Zanna.Collections.BiMap.New()
PRINT "bm empty: "; bm.IsEmpty
bm.Set("one", "1")
bm.Set("two", "2")
PRINT "bm len: "; bm.Count
PRINT "bm getbykey one: "; bm.GetByKey("one")
PRINT "bm getbyvalue 2: "; bm.GetByValue("2")
PRINT "bm haskey one: "; bm.HasKey("one")
PRINT "bm hasvalue 2: "; bm.HasValue("2")
bm.RemoveByKey("one")
PRINT "bm len after remove: "; bm.Count

PRINT "done"
END
