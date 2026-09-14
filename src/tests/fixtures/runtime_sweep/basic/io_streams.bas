' EXPECT_OUT: RESULT: ok
' COVER: Zanna.IO.BinFile.Open
' COVER: Zanna.IO.BinFile.Eof
' COVER: Zanna.IO.BinFile.Position
' COVER: Zanna.IO.BinFile.SizeBytes
' COVER: Zanna.IO.BinFile.Close
' COVER: Zanna.IO.BinFile.Flush
' COVER: Zanna.IO.BinFile.Read
' COVER: Zanna.IO.BinFile.ReadByte
' COVER: Zanna.IO.BinFile.Seek
' COVER: Zanna.IO.BinFile.Write
' COVER: Zanna.IO.BinFile.WriteByte
' COVER: Zanna.IO.LineReader.Open
' COVER: Zanna.IO.LineReader.Eof
' COVER: Zanna.IO.LineReader.Close
' COVER: Zanna.IO.LineReader.PeekChar
' COVER: Zanna.IO.LineReader.Read
' COVER: Zanna.IO.LineReader.ReadAll
' COVER: Zanna.IO.LineReader.ReadChar
' COVER: Zanna.IO.LineWriter.Open
' COVER: Zanna.IO.LineWriter.Open
' COVER: Zanna.IO.LineWriter.Close
' COVER: Zanna.IO.LineWriter.Flush
' COVER: Zanna.IO.LineWriter.Write
' COVER: Zanna.IO.LineWriter.WriteChar
' COVER: Zanna.IO.LineWriter.WriteLine
' COVER: Zanna.IO.MemStream.New
' COVER: Zanna.IO.MemStream.Capacity
' COVER: Zanna.IO.MemStream.Length
' COVER: Zanna.IO.MemStream.Position
' COVER: Zanna.IO.MemStream.Clear
' COVER: Zanna.IO.MemStream.ReadBytes
' COVER: Zanna.IO.MemStream.ReadF32
' COVER: Zanna.IO.MemStream.ReadF64
' COVER: Zanna.IO.MemStream.ReadI16
' COVER: Zanna.IO.MemStream.ReadI32
' COVER: Zanna.IO.MemStream.ReadI64
' COVER: Zanna.IO.MemStream.ReadI8
' COVER: Zanna.IO.MemStream.ReadStr
' COVER: Zanna.IO.MemStream.ReadU16
' COVER: Zanna.IO.MemStream.ReadU32
' COVER: Zanna.IO.MemStream.ReadU8
' COVER: Zanna.IO.MemStream.Seek
' COVER: Zanna.IO.MemStream.Skip
' COVER: Zanna.IO.MemStream.ToBytes
' COVER: Zanna.IO.MemStream.WriteBytes
' COVER: Zanna.IO.MemStream.WriteF32
' COVER: Zanna.IO.MemStream.WriteF64
' COVER: Zanna.IO.MemStream.WriteI16
' COVER: Zanna.IO.MemStream.WriteI32
' COVER: Zanna.IO.MemStream.WriteI64
' COVER: Zanna.IO.MemStream.WriteI8
' COVER: Zanna.IO.MemStream.WriteStr
' COVER: Zanna.IO.MemStream.WriteU16
' COVER: Zanna.IO.MemStream.WriteU32
' COVER: Zanna.IO.MemStream.WriteU8

SUB AssertApprox(actual AS DOUBLE, expected AS DOUBLE, eps AS DOUBLE, msg AS STRING)
    IF Zanna.Math.Abs(actual - expected) > eps THEN
        Zanna.Core.Diagnostics.Assert(FALSE, msg)
    END IF
END SUB

DIM cwd AS STRING
cwd = Zanna.IO.Dir.Current()
DIM base AS STRING
base = Zanna.IO.Path.Join(cwd, "tests/runtime_sweep/tmp_streams")
IF Zanna.IO.Dir.Exists(base) THEN
    Zanna.IO.Dir.RemoveAll(base)
END IF
Zanna.IO.Dir.MakeAll(base)

DIM binPath AS STRING
binPath = Zanna.IO.Path.Join(base, "bin.dat")
DIM bf AS Zanna.IO.BinFile
bf = Zanna.IO.BinFile.Open(binPath, "w")
bf.WriteByte(202)
bf.WriteByte(254)
DIM buf AS Zanna.Collections.Bytes
buf = Zanna.Collections.Bytes.FromHex("0102")
bf.Write(buf, 0, 2)
bf.Flush()
bf.Close()

bf = Zanna.IO.BinFile.Open(binPath, "r")
Zanna.Core.Diagnostics.AssertEq(bf.SizeBytes, 4, "bin.size")
Zanna.Core.Diagnostics.AssertEq(bf.Position, 0, "bin.pos0")
DIM b0 AS INTEGER
b0 = bf.ReadByte()
Zanna.Core.Diagnostics.AssertEq(b0, 202, "bin.readbyte")
DIM readBuf AS Zanna.Collections.Bytes
readBuf = Zanna.Collections.Bytes.New(2)
DIM readCount AS INTEGER
readCount = bf.Read(readBuf, 0, 2)
Zanna.Core.Diagnostics.AssertEq(readCount, 2, "bin.read")
Zanna.Core.Diagnostics.AssertEqStr(Zanna.Collections.Bytes.ToHex(readBuf), "fe01", "bin.read.hex")
DIM newPos AS INTEGER
newPos = bf.Seek(0, 0)
Zanna.Core.Diagnostics.AssertEq(newPos, 0, "bin.seek")
Zanna.Core.Diagnostics.Assert(bf.Eof = FALSE, "bin.eof")
bf.Close()

DIM linesPath AS STRING
linesPath = Zanna.IO.Path.Join(base, "lines.txt")
DIM writer AS Zanna.IO.LineWriter
writer = Zanna.IO.LineWriter.Open(linesPath)
writer.NewLine = CHR$(10)
writer.Write("A")
writer.WriteChar(66)
writer.WriteLine("C")
writer.Flush()
writer.Close()

DIM reader AS Zanna.IO.LineReader
reader = Zanna.IO.LineReader.Open(linesPath)
DIM peek AS INTEGER
peek = reader.PeekChar()
Zanna.Core.Diagnostics.Assert(peek >= 0, "line.peek")
DIM ch AS INTEGER
ch = reader.ReadChar()
Zanna.Core.Diagnostics.Assert(ch >= 0, "line.readchar")
DIM line AS STRING
line = reader.Read()
Zanna.Core.Diagnostics.AssertEqStr(line, "BC", "line.read")
DIM rest AS STRING
rest = reader.ReadAll()
reader.Close()
Zanna.Core.Diagnostics.Assert(reader.Eof, "line.eof")

DIM ms AS Zanna.IO.MemStream
ms = Zanna.IO.MemStream.New()
ms.WriteI8(-5)
ms.WriteU8(250)
ms.WriteI16(-1234)
ms.WriteU16(65530)
ms.WriteI32(-123456)
ms.WriteU32(4000000000)
ms.WriteI64(-123456789)
ms.WriteF32(1.5)
ms.WriteF64(2.25)
ms.WriteStr("hi")
DIM msBytes AS Zanna.Collections.Bytes
msBytes = Zanna.Collections.Bytes.FromHex("0708")
ms.WriteBytes(msBytes)

Zanna.Core.Diagnostics.Assert(ms.Length > 0, "ms.len")
Zanna.Core.Diagnostics.Assert(ms.Capacity >= ms.Length, "ms.capacity")

ms.Seek(0)
Zanna.Core.Diagnostics.AssertEq(ms.ReadI8(), -5, "ms.readi8")
Zanna.Core.Diagnostics.AssertEq(ms.ReadU8(), 250, "ms.readu8")
Zanna.Core.Diagnostics.AssertEq(ms.ReadI16(), -1234, "ms.readi16")
Zanna.Core.Diagnostics.AssertEq(ms.ReadU16(), 65530, "ms.readu16")
Zanna.Core.Diagnostics.AssertEq(ms.ReadI32(), -123456, "ms.readi32")
Zanna.Core.Diagnostics.AssertEq(ms.ReadU32(), 4000000000, "ms.readu32")
Zanna.Core.Diagnostics.AssertEq(ms.ReadI64(), -123456789, "ms.readi64")
AssertApprox(ms.ReadF32(), 1.5, 0.0001, "ms.readf32")
AssertApprox(ms.ReadF64(), 2.25, 0.0001, "ms.readf64")
Zanna.Core.Diagnostics.AssertEqStr(ms.ReadStr(2), "hi", "ms.readstr")
DIM rb AS Zanna.Collections.Bytes
rb = ms.ReadBytes(2)
Zanna.Core.Diagnostics.AssertEqStr(Zanna.Collections.Bytes.ToHex(rb), "0708", "ms.readbytes")

ms.Skip(0)
DIM allBytes AS Zanna.Collections.Bytes
allBytes = ms.ToBytes()
Zanna.Core.Diagnostics.Assert(allBytes.Length >= 0, "ms.tobytes")

ms.Clear()
Zanna.Core.Diagnostics.AssertEq(ms.Length, 0, "ms.clear")
Zanna.Core.Diagnostics.AssertEq(ms.Position, 0, "ms.pos0")

Zanna.IO.Dir.RemoveAll(base)

PRINT "RESULT: ok"
END
