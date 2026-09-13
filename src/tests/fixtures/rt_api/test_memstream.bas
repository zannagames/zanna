' test_memstream.bas — IO.MemStream, IO.BinFile, IO.LineReader, IO.LineWriter, IO.Compress

' --- MemStream: integer read/write ---
DIM ms AS Zanna.IO.MemStream
ms = Zanna.IO.MemStream.New()
PRINT "ms pos: "; ms.Position
PRINT "ms len: "; ms.Length

ms.WriteI8(42)
ms.WriteI16(1000)
ms.WriteI32(100000)
ms.WriteI64(9999999999)
PRINT "ms len after writes: "; ms.Length

' Seek back to start and read
ms.Seek(0)
PRINT "read i8: "; ms.ReadI8()
PRINT "read i16: "; ms.ReadI16()
PRINT "read i32: "; ms.ReadI32()
PRINT "read i64: "; ms.ReadI64()

' --- MemStream: string read/write ---
ms.Clear()
ms.WriteStr("hello world")
PRINT "ms len after str: "; ms.Length
ms.Seek(0)
PRINT "read str: "; ms.ReadStr(11)

' --- MemStream: unsigned types ---
ms.Clear()
ms.WriteU8(200)
ms.WriteU16(60000)
ms.WriteU32(3000000000)
ms.Seek(0)
PRINT "read u8: "; ms.ReadU8()
PRINT "read u16: "; ms.ReadU16()
PRINT "read u32: "; ms.ReadU32()

' --- MemStream: capacity and skip ---
PRINT "ms cap > 0: "; (ms.Capacity > 0)
ms.Clear()
ms.WriteI8(1)
ms.WriteI8(2)
ms.WriteI8(3)
ms.Seek(0)
ms.Skip(1)
PRINT "after skip pos: "; ms.Position

' --- Text streams via runtime constructor aliases ---
DIM textPath AS STRING
DIM writer AS OBJECT
DIM reader AS OBJECT
textPath = "/tmp/zanna_rt_api_memstream_lines.txt"
writer = Zanna.IO.LineWriter.Open(textPath)
writer.Write("alpha")
writer.WriteLine("beta")
writer.Close()

reader = Zanna.IO.LineReader.Open(textPath)
PRINT "peek char: "; reader.PeekChar()
PRINT "line 1: "; reader.Read()
PRINT "line 2: "; reader.Read()
PRINT "reader eof: "; reader.Eof
reader.Close()

' --- Binary file via runtime constructor alias ---
DIM binPath AS STRING
DIM wf AS OBJECT
DIM rf AS OBJECT
binPath = "/tmp/zanna_rt_api_memstream.bin"
wf = Zanna.IO.BinFile.Open(binPath, "w")
wf.WriteByte(65)
wf.WriteByte(66)
wf.Close()

rf = Zanna.IO.BinFile.Open(binPath, "r")
PRINT "byte 1: "; rf.ReadByte()
PRINT "byte 2: "; rf.ReadByte()
PRINT "bin eof: "; rf.Eof
rf.Close()

' --- Compress: string deflate/inflate ---
DIM compressed AS OBJECT
compressed = Zanna.IO.Compress.DeflateStr("hello world hello world hello world")
PRINT "compressed is obj: true"

DIM decompressed AS STRING
decompressed = Zanna.IO.Compress.InflateStr(compressed)
PRINT "inflate: "; decompressed

' Gzip round-trip
DIM gzipped AS OBJECT
gzipped = Zanna.IO.Compress.GzipStr("test data for gzip")
DIM gunzipped AS STRING
gunzipped = Zanna.IO.Compress.GunzipStr(gzipped)
PRINT "gunzip: "; gunzipped

PRINT "done"
END
