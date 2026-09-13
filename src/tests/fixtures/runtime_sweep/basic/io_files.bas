' EXPECT_OUT: RESULT: ok
' COVER: Zanna.IO.Dir.Current
' COVER: Zanna.IO.Dir.Dirs
' COVER: Zanna.IO.Dir.Dirs
' COVER: Zanna.IO.Dir.Entries
' COVER: Zanna.IO.Dir.Exists
' COVER: Zanna.IO.Dir.Files
' COVER: Zanna.IO.Dir.Files
' COVER: Zanna.IO.Dir.List
' COVER: Zanna.IO.Dir.List
' COVER: Zanna.IO.Dir.Page
' COVER: Zanna.IO.Dir.Make
' COVER: Zanna.IO.Dir.MakeAll
' COVER: Zanna.IO.Dir.Move
' COVER: Zanna.IO.Dir.Remove
' COVER: Zanna.IO.Dir.RemoveAll
' COVER: Zanna.IO.Dir.SetCurrent
' COVER: Zanna.IO.File.Append
' COVER: Zanna.IO.File.AppendLine
' COVER: Zanna.IO.File.Copy
' COVER: Zanna.IO.File.Delete
' COVER: Zanna.IO.File.Exists
' COVER: Zanna.IO.File.Modified
' COVER: Zanna.IO.File.Move
' COVER: Zanna.IO.File.ReadAllBytes
' COVER: Zanna.IO.File.ReadAllLines
' COVER: Zanna.IO.File.ReadAllText
' COVER: Zanna.IO.File.ReadAllBytes
' COVER: Zanna.IO.File.ReadAllLines
' COVER: Zanna.IO.File.SizeBytes
' COVER: Zanna.IO.File.Touch
' COVER: Zanna.IO.File.WriteAllBytes
' COVER: Zanna.IO.File.WriteAllText
' COVER: Zanna.IO.File.WriteAllBytes
' COVER: Zanna.IO.File.WriteAllLines
' COVER: Zanna.IO.Path.Absolute
' COVER: Zanna.IO.Path.Directory
' COVER: Zanna.IO.Path.Extension
' COVER: Zanna.IO.Path.IsAbsolute
' COVER: Zanna.IO.Path.Join
' COVER: Zanna.IO.Path.Name
' COVER: Zanna.IO.Path.Normalize
' COVER: Zanna.IO.Path.Separator
' COVER: Zanna.IO.Path.Stem
' COVER: Zanna.IO.Path.WithExtension

DIM cwd AS STRING
cwd = Zanna.IO.Dir.Current()
DIM base AS STRING
base = Zanna.IO.Path.Join(cwd, "tests/runtime_sweep/tmp_io")
IF Zanna.IO.Dir.Exists(base) THEN
    Zanna.IO.Dir.RemoveAll(base)
END IF
Zanna.IO.Dir.MakeAll(base)

DIM subdir AS STRING
subdir = Zanna.IO.Path.Join(base, "sub")
Zanna.IO.Dir.Make(subdir)
Zanna.Core.Diagnostics.Assert(Zanna.IO.Dir.Exists(subdir), "dir.exists")

DIM fileA AS STRING
fileA = Zanna.IO.Path.Join(base, "a.txt")
Zanna.IO.File.WriteAllText(fileA, "one")
Zanna.IO.File.Append(fileA, " two")
Zanna.IO.File.AppendLine(fileA, "three")
DIM text AS STRING
text = Zanna.IO.File.ReadAllText(fileA)
Zanna.Core.Diagnostics.Assert(Zanna.String.Contains(text, "one"), "file.readalltext")

DIM lines AS Zanna.Collections.Seq
lines = Zanna.Collections.Seq.New()
lines.Push("L1")
lines.Push("L2")
DIM fileB AS STRING
fileB = Zanna.IO.Path.Join(base, "b.txt")
Zanna.IO.File.WriteAllLines(fileB, lines)
DIM linesAll AS Zanna.Collections.Seq
linesAll = Zanna.IO.File.ReadAllLines(fileB)
Zanna.Core.Diagnostics.AssertEq(linesAll.Count, 2, "file.readalllines")
DIM linesSeq AS Zanna.Collections.Seq
linesSeq = Zanna.IO.File.ReadAllLines(fileB)
Zanna.Core.Diagnostics.AssertEq(linesSeq.Count, 2, "file.readlines")

DIM bytes AS Zanna.Collections.Bytes
bytes = Zanna.Collections.Bytes.FromHex("010203")
DIM fileBin AS STRING
fileBin = Zanna.IO.Path.Join(base, "c.bin")
Zanna.IO.File.WriteAllBytes(fileBin, bytes)
DIM readAll AS Zanna.Collections.Bytes
readAll = Zanna.IO.File.ReadAllBytes(fileBin)
Zanna.Core.Diagnostics.AssertEqStr(Zanna.Collections.Bytes.ToHex(readAll), "010203", "file.readallbytes")
Zanna.IO.File.WriteAllBytes(fileBin, bytes)
DIM readBytes AS Zanna.Collections.Bytes
readBytes = Zanna.IO.File.ReadAllBytes(fileBin)
Zanna.Core.Diagnostics.AssertEqStr(Zanna.Collections.Bytes.ToHex(readBytes), "010203", "file.readbytes")

Zanna.Core.Diagnostics.AssertEq(Zanna.IO.File.SizeBytes(fileBin), 3, "file.size")
DIM mod1 AS INTEGER
mod1 = Zanna.IO.File.Modified(fileBin)
Zanna.IO.File.Touch(fileBin)
DIM mod2 AS INTEGER
mod2 = Zanna.IO.File.Modified(fileBin)
Zanna.Core.Diagnostics.Assert(mod2 >= mod1, "file.modified")

DIM copyPath AS STRING
copyPath = Zanna.IO.Path.Join(base, "copy.bin")
Zanna.IO.File.Copy(fileBin, copyPath)
Zanna.Core.Diagnostics.Assert(Zanna.IO.File.Exists(copyPath), "file.copy")
DIM movePath AS STRING
movePath = Zanna.IO.Path.Join(base, "moved.bin")
Zanna.IO.File.Move(copyPath, movePath)
Zanna.Core.Diagnostics.Assert(Zanna.IO.File.Exists(movePath), "file.move")
Zanna.IO.File.Delete(movePath)
Zanna.Core.Diagnostics.Assert(Zanna.IO.File.Exists(movePath) = FALSE, "file.delete")

DIM entries AS Zanna.Collections.Seq
entries = Zanna.IO.Dir.Entries(base)
DIM list1 AS Zanna.Collections.Seq
list1 = Zanna.IO.Dir.List(base)
DIM list2 AS Zanna.Collections.Seq
list2 = Zanna.IO.Dir.List(base)
DIM files1 AS Zanna.Collections.Seq
files1 = Zanna.IO.Dir.Files(base)
DIM files2 AS Zanna.Collections.Seq
files2 = Zanna.IO.Dir.Files(base)
DIM dirs1 AS Zanna.Collections.Seq
dirs1 = Zanna.IO.Dir.Dirs(base)
DIM dirs2 AS Zanna.Collections.Seq
dirs2 = Zanna.IO.Dir.Dirs(base)
DIM dirPage AS Zanna.Collections.Map
dirPage = Zanna.IO.Dir.Page(base, 0, 1)
Zanna.Core.Diagnostics.Assert(entries.Count >= 2, "dir.entries")
Zanna.Core.Diagnostics.Assert(list1.Count = list2.Count, "dir.list")
Zanna.Core.Diagnostics.Assert(files1.Count = files2.Count, "dir.files")
Zanna.Core.Diagnostics.Assert(dirs1.Count = dirs2.Count, "dir.dirs")
Zanna.Core.Diagnostics.Assert(Zanna.Collections.Map.GetBool(dirPage, "valid"), "dir.page.valid")
Zanna.Core.Diagnostics.Assert(Zanna.Collections.Map.GetInt(dirPage, "emitted") = 1, "dir.page.emitted")

DIM baseMoved AS STRING
baseMoved = Zanna.IO.Path.Join(cwd, "tests/runtime_sweep/tmp_io_moved")
IF Zanna.IO.Dir.Exists(baseMoved) THEN
    Zanna.IO.Dir.RemoveAll(baseMoved)
END IF
Zanna.IO.Dir.Move(base, baseMoved)
Zanna.Core.Diagnostics.Assert(Zanna.IO.Dir.Exists(baseMoved), "dir.move")
Zanna.IO.Dir.RemoveAll(baseMoved)
Zanna.Core.Diagnostics.Assert(Zanna.IO.Dir.Exists(baseMoved) = FALSE, "dir.removeall")

DIM emptyDir AS STRING
emptyDir = Zanna.IO.Path.Join(cwd, "tests/runtime_sweep/tmp_empty")
IF Zanna.IO.Dir.Exists(emptyDir) THEN
    Zanna.IO.Dir.RemoveAll(emptyDir)
END IF
Zanna.IO.Dir.Make(emptyDir)
Zanna.IO.Dir.Remove(emptyDir)
Zanna.Core.Diagnostics.Assert(Zanna.IO.Dir.Exists(emptyDir) = FALSE, "dir.remove")

Zanna.IO.Dir.MakeAll(base)
DIM cur AS STRING
cur = Zanna.IO.Dir.Current()
Zanna.IO.Dir.SetCurrent(base)
Zanna.Core.Diagnostics.AssertEqStr(Zanna.IO.Dir.Current(), base, "dir.setcurrent")
Zanna.IO.Dir.SetCurrent(cur)

DIM absPath AS STRING
absPath = Zanna.IO.Path.Absolute("tests")
Zanna.Core.Diagnostics.Assert(Zanna.IO.Path.IsAbsolute(absPath), "path.abs")
Zanna.Core.Diagnostics.AssertEqStr(Zanna.IO.Path.Directory(fileA), base, "path.dir")
Zanna.Core.Diagnostics.AssertEqStr(Zanna.IO.Path.Name(fileA), "a.txt", "path.name")
Zanna.Core.Diagnostics.AssertEqStr(Zanna.IO.Path.Stem(fileA), "a", "path.stem")
Zanna.Core.Diagnostics.AssertEqStr(Zanna.IO.Path.Extension(fileA), ".txt", "path.ext")
DIM withExt AS STRING
withExt = Zanna.IO.Path.WithExtension(fileA, ".md")
Zanna.Core.Diagnostics.AssertEqStr(withExt, Zanna.IO.Path.Join(base, "a.md"), "path.withext")
Zanna.Core.Diagnostics.AssertEqStr(Zanna.IO.Path.Normalize("a/b/../c"), "a/c", "path.norm")
DIM sep AS STRING
sep = Zanna.IO.Path.Separator()
Zanna.Core.Diagnostics.Assert(sep <> "", "path.sep")

IF Zanna.IO.Dir.Exists(base) THEN
    Zanna.IO.Dir.RemoveAll(base)
END IF

PRINT "RESULT: ok"
END
