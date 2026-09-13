' test_list_seq.bas — Zanna.Collections.List + Seq
DIM l AS Zanna.Collections.List
l = Zanna.Collections.List.New()
l.Push("a")
l.Push("b")
l.Push("c")
PRINT "len: "; l.Count
PRINT "find b: "; l.FindOption("b").IsSome
PRINT "has b: "; l.Has("b")
l.Set(1, "B")
l.Insert(0, "z")
PRINT "len after insert: "; l.Count
l.RemoveAt(0)
PRINT "len after removeat: "; l.Count
l.Remove("a")
PRINT "len after remove: "; l.Count
l.Reverse()
l.Clear()
PRINT "len after clear: "; l.Count

DIM s AS Zanna.Collections.Seq
s = Zanna.Collections.Seq.New()
s.Push("x")
s.Push("y")
s.Push("z")
PRINT "seq len: "; s.Count
PRINT "seq find y: "; s.FindOption("y").IsSome
PRINT "seq has y: "; s.Has("y")
PRINT "seq isempty: "; s.IsEmpty
s.Reverse()
DIM s2 AS Zanna.Collections.Seq
s2 = s.Clone()
PRINT "clone len: "; s2.Count
DIM s3 AS Zanna.Collections.Seq
s3 = s.Slice(0, 2)
PRINT "slice len: "; s3.Count
DIM s4 AS Zanna.Collections.Seq
s4 = s.Take(2)
PRINT "take len: "; s4.Count
DIM s5 AS Zanna.Collections.Seq
s5 = s.Drop(1)
PRINT "drop len: "; s5.Count
s.Sort()
s.SortDesc()
s.Clear()
PRINT "seq len after clear: "; s.Count
PRINT "seq isempty: "; s.IsEmpty
PRINT "done"
END
