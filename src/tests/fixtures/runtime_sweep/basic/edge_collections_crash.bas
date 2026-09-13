' EXPECT_EXIT: 1
' EXPECT_OUT: Test: Seq.Get(-1)
' EXPECT_ERR: Seq.Get: index out of bounds
' Operations that could crash must trap cleanly instead.

' === Test: Seq.Get negative index traps ===
PRINT "Test: Seq.Get(-1)"
DIM seq AS Zanna.Collections.Seq
seq = Zanna.Collections.Seq.New()
seq.Push("a")
seq.Push("b")
Zanna.Core.Diagnostics.AssertEqStr(Zanna.Core.Box.ToStr(seq.Get(-1)), "???", "get -1")
PRINT "FAIL: Get(-1) did not trap"
END
