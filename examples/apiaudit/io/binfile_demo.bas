' =============================================================================
' API Audit: Zanna.IO.BinFile - Binary File I/O
' =============================================================================
' Tests: Open, Close, ReadByte, WriteByte, Read, Write, Seek, Position, SizeBytes, Eof, Flush
' =============================================================================

PRINT "=== API Audit: Zanna.IO.BinFile ==="

DIM tmpDir AS STRING
tmpDir = Zanna.IO.TempFile.Dir()
DIM testPath AS STRING
testPath = Zanna.IO.Path.Join(tmpDir, "zanna_binfile_audit_bas.bin")

' --- Open (write) + WriteByte ---
PRINT "--- Open (write) + WriteByte ---"
DIM wf AS OBJECT
wf = Zanna.IO.BinFile.Open(testPath, "w")
Zanna.IO.BinFile.WriteByte(wf, 65)
Zanna.IO.BinFile.WriteByte(wf, 66)
Zanna.IO.BinFile.WriteByte(wf, 67)
PRINT "Wrote 3 bytes: A B C"

' --- Position ---
PRINT "--- Position ---"
PRINT "Pos after write: "; Zanna.IO.BinFile.get_Position(wf)

' --- Flush ---
PRINT "--- Flush ---"
Zanna.IO.BinFile.Flush(wf)
PRINT "Flush done"

' --- Close ---
PRINT "--- Close ---"
Zanna.IO.BinFile.Close(wf)
PRINT "Close done"

' --- Open (read) ---
PRINT "--- Open (read) ---"
DIM rf AS OBJECT
rf = Zanna.IO.BinFile.Open(testPath, "r")

' --- SizeBytes ---
PRINT "--- SizeBytes ---"
PRINT "Size: "; Zanna.IO.BinFile.get_SizeBytes(rf)

' --- ReadByte ---
PRINT "--- ReadByte ---"
PRINT "ReadByte 1: "; Zanna.IO.BinFile.ReadByte(rf)
PRINT "ReadByte 2: "; Zanna.IO.BinFile.ReadByte(rf)
PRINT "ReadByte 3: "; Zanna.IO.BinFile.ReadByte(rf)

' --- Seek ---
PRINT "--- Seek ---"
Zanna.IO.BinFile.Seek(rf, 0, 0)
PRINT "Seek to 0, ReadByte: "; Zanna.IO.BinFile.ReadByte(rf)

' --- Eof ---
PRINT "--- Eof ---"
Zanna.IO.BinFile.Seek(rf, 0, 2)
PRINT "Eof at end: "; Zanna.IO.BinFile.get_Eof(rf)

Zanna.IO.BinFile.Close(rf)

' Cleanup
Zanna.IO.File.Delete(testPath)
PRINT "Cleanup done"

PRINT "=== BinFile Demo Complete ==="
END
