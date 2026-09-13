' API Audit: Zanna.IO.Stream (BASIC)
PRINT "=== API Audit: Zanna.IO.Stream ==="

' --- OpenMemory ---
PRINT "--- OpenMemory ---"
DIM s1 AS OBJECT = Zanna.IO.Stream.OpenMemory()
PRINT s1.Type
PRINT s1.Position
PRINT s1.Length

' --- WriteByte / ReadByte ---
PRINT "--- WriteByte / ReadByte ---"
s1.WriteByte(65)
s1.WriteByte(66)
s1.WriteByte(67)
PRINT s1.Length
s1.Position = 0
PRINT s1.ReadByte()
PRINT s1.ReadByte()
PRINT s1.ReadByte()

' --- Write / Read ---
PRINT "--- Write / Read ---"
DIM s2 AS OBJECT = Zanna.IO.Stream.OpenMemory()
DIM data AS OBJECT = Zanna.Collections.Bytes.New(4)
data.Set(0, 72)
data.Set(1, 73)
data.Set(2, 33)
data.Set(3, 10)
s2.Write(data)
PRINT s2.Length
s2.Position = 0
DIM rd AS OBJECT = s2.Read(4)
PRINT Zanna.Collections.Bytes.Get(rd, 0)
PRINT Zanna.Collections.Bytes.Get(rd, 1)

' --- Eof ---
PRINT "--- Eof ---"
PRINT s2.Eof
s2.Position = 0
PRINT s2.Eof

' --- ReadAll ---
PRINT "--- ReadAll ---"
s2.Position = 0
DIM all_data AS OBJECT = s2.ReadAll()
PRINT all_data.Length

' --- ToBytes ---
PRINT "--- ToBytes ---"
DIM b AS OBJECT = s1.ToBytes()
PRINT b.Length

' --- Flush / Close ---
PRINT "--- Flush / Close ---"
s1.Flush()
s1.Close()
s2.Close()
PRINT "Closed all streams"

PRINT "=== Stream Audit Complete ==="
END
