' bytes_demo.bas - Comprehensive API audit for Zanna.Collections.Bytes
' Tests: New, Get, Set, Len, Fill, Find, Clone, Slice, Copy,
'        ToHex, FromHex, ToBase64, FromBase64, ToStr, FromStr

PRINT "=== Bytes API Audit ==="

' --- New ---
PRINT "--- New ---"
DIM b AS Zanna.Collections.Bytes
b = Zanna.Collections.Bytes.New(4)
PRINT b.Length           ' 4

' --- Set / Get ---
PRINT "--- Set / Get ---"
b.Set(0, 72)   ' H
b.Set(1, 101)  ' e
b.Set(2, 108)  ' l
b.Set(3, 108)  ' l
PRINT b.Get(0)        ' 72
PRINT b.Get(1)        ' 101
PRINT b.Get(2)        ' 108
PRINT b.Get(3)        ' 108

' --- FromStr / ToStr ---
PRINT "--- FromStr / ToStr ---"
DIM hello AS Zanna.Collections.Bytes
hello = Zanna.Collections.Bytes.FromStr("Hello")
PRINT hello.Length       ' 5
PRINT hello.ToStr()   ' Hello
PRINT hello.Get(0)    ' 72

' --- ToHex ---
PRINT "--- ToHex ---"
PRINT hello.ToHex()   ' 48656c6c6f

' --- FromHex ---
PRINT "--- FromHex ---"
DIM hex AS Zanna.Collections.Bytes
hex = Zanna.Collections.Bytes.FromHex("deadbeef")
PRINT hex.Length         ' 4
PRINT hex.Get(0)      ' 222
PRINT hex.Get(3)      ' 239

' --- ToBase64 ---
PRINT "--- ToBase64 ---"
PRINT hello.ToBase64()   ' SGVsbG8=

' --- FromBase64 ---
PRINT "--- FromBase64 ---"
DIM decoded AS Zanna.Collections.Bytes
decoded = Zanna.Collections.Bytes.FromBase64("SGVsbG8=")
PRINT decoded.ToStr()    ' Hello
PRINT decoded.Length        ' 5

' --- Fill ---
PRINT "--- Fill ---"
DIM fill AS Zanna.Collections.Bytes
fill = Zanna.Collections.Bytes.New(4)
fill.Fill(255)
PRINT fill.Get(0)        ' 255
PRINT fill.Get(3)        ' 255
PRINT fill.ToHex()       ' ffffffff

' --- Find ---
PRINT "--- Find ---"
PRINT hello.Find(108).UnwrapOrI64(-1)    ' 2
PRINT hello.Find(111).UnwrapOrI64(-1)    ' 4
PRINT hello.Find(99).UnwrapOrI64(-1)     ' -1
DIM found AS OBJECT
found = hello.Find(108)
PRINT found.IsSome
PRINT found.UnwrapI64()  ' 2
PRINT hello.Find(99).IsNone

' --- Clone ---
PRINT "--- Clone ---"
DIM clone AS Zanna.Collections.Bytes
clone = hello.Clone()
PRINT clone.Length          ' 5
PRINT clone.ToStr()      ' Hello
clone.Set(0, 74)
PRINT clone.ToStr()      ' Jello
PRINT hello.ToStr()      ' Hello (unchanged)

' --- Slice ---
PRINT "--- Slice ---"
DIM sl AS Zanna.Collections.Bytes
sl = hello.Slice(1, 4)
PRINT sl.Length             ' 3
PRINT sl.ToStr()         ' ell

' --- Copy ---
PRINT "--- Copy ---"
DIM dst AS Zanna.Collections.Bytes
dst = Zanna.Collections.Bytes.New(10)
dst.Fill(0)
dst.Copy(2, hello, 0, 5)
PRINT dst.Get(0)         ' 0
PRINT dst.Get(1)         ' 0
PRINT dst.Get(2)         ' 72
PRINT dst.Get(6)         ' 111

PRINT "=== Bytes audit complete ==="
END
