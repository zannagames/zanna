' unionfind_demo.bas - Comprehensive API audit for Zanna.Collections.UnionFind
' Tests: New, FindRoot, Union, IsConnected, Count, ComponentSize, Clear

PRINT "=== UnionFind API Audit ==="

' --- New ---
PRINT "--- New ---"
DIM uf AS OBJECT
uf = Zanna.Collections.UnionFind.New(6)
PRINT uf.Count       ' 6

' --- FindRoot ---
PRINT "--- FindRoot ---"
PRINT uf.FindRoot(0).UnwrapOrI64(-1)     ' 0
PRINT uf.FindRoot(3).UnwrapOrI64(-1)     ' 3
DIM root AS OBJECT
root = uf.FindRoot(0)
PRINT root.IsSome
PRINT root.UnwrapI64()  ' 0
PRINT uf.FindRoot(99).IsNone

' --- Union ---
PRINT "--- Union ---"
PRINT uf.Union(0, 1)  ' 1 (merged)
PRINT uf.Count         ' 5
PRINT uf.Union(2, 3)  ' 1 (merged)
PRINT uf.Count         ' 4
PRINT uf.Union(0, 1)  ' 0 (already same set)
PRINT uf.Count         ' 4

' --- IsConnected ---
PRINT "--- IsConnected ---"
PRINT uf.IsConnected(0, 1)  ' 1
PRINT uf.IsConnected(2, 3)  ' 1
PRINT uf.IsConnected(0, 2)  ' 0
PRINT uf.IsConnected(4, 5)  ' 0

' --- Union across groups ---
PRINT "--- Union across groups ---"
PRINT uf.Union(1, 3)      ' 1
PRINT uf.Count             ' 3
PRINT uf.IsConnected(0, 2)  ' 1
PRINT uf.IsConnected(0, 3)  ' 1

' --- ComponentSize ---
PRINT "--- ComponentSize ---"
PRINT uf.ComponentSize(0)       ' 4
PRINT uf.ComponentSize(4)       ' 1
PRINT uf.ComponentSize(5)       ' 1

' --- Union remaining ---
PRINT "--- Union remaining ---"
uf.Union(4, 5)
PRINT uf.Count             ' 2
PRINT uf.ComponentSize(4)        ' 2

uf.Union(0, 4)
PRINT uf.Count             ' 1
PRINT uf.ComponentSize(0)        ' 6

' --- Clear ---
PRINT "--- Clear ---"
uf.Clear()
PRINT uf.Count             ' 6
PRINT uf.IsConnected(0, 1)  ' 0
PRINT uf.ComponentSize(0)        ' 1

PRINT "=== UnionFind audit complete ==="
END
