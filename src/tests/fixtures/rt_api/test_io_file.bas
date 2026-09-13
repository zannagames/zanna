' test_io_file.bas — IO.File, IO.TempFile, IO.Dir, IO.Glob
' Tests actual file operations: create, read, write, copy, move, delete

' --- TempFile: get temp directory and create temp paths ---
DIM tmpdir AS STRING
tmpdir = Zanna.IO.TempFile.Dir()
PRINT "tempdir nonempty: "; (LEN(tmpdir) > 0)

DIM tmppath AS STRING
tmppath = Zanna.IO.TempFile.Path()
PRINT "tmppath nonempty: "; (LEN(tmppath) > 0)

DIM tmppfx AS STRING
tmppfx = Zanna.IO.TempFile.PathWithPrefix("vtest")
PRINT "tmppfx has prefix: "; Zanna.String.Contains(tmppfx, "vtest")

DIM tmpext AS STRING
tmpext = Zanna.IO.TempFile.PathWithExt("vtest", ".txt")
PRINT "tmpext has ext: "; Zanna.String.EndsWith(tmpext, ".txt")

' --- File: write, read, exists, size ---
DIM testfile AS STRING
testfile = Zanna.IO.TempFile.PathWithExt("vtest_io", ".txt")

Zanna.IO.File.WriteAllText(testfile, "hello world")
PRINT "file exists: "; Zanna.IO.File.Exists(testfile)
PRINT "file size: "; Zanna.IO.File.SizeBytes(testfile)

DIM content AS STRING
content = Zanna.IO.File.ReadAllText(testfile)
PRINT "file content: "; content

' --- File: append ---
Zanna.IO.File.Append(testfile, " appended")
content = Zanna.IO.File.ReadAllText(testfile)
PRINT "after append: "; content

' --- File: appendline ---
Zanna.IO.File.AppendLine(testfile, "line2")
content = Zanna.IO.File.ReadAllText(testfile)
PRINT "content has line2: "; Zanna.String.Contains(content, "line2")

' --- File: copy ---
DIM copyfile AS STRING
copyfile = Zanna.IO.TempFile.PathWithExt("vtest_copy", ".txt")
Zanna.IO.File.Copy(testfile, copyfile)
PRINT "copy exists: "; Zanna.IO.File.Exists(copyfile)

DIM copycontent AS STRING
copycontent = Zanna.IO.File.ReadAllText(copyfile)
PRINT "copy matches: "; (copycontent = content)

' --- File: move ---
DIM movefile AS STRING
movefile = Zanna.IO.TempFile.PathWithExt("vtest_moved", ".txt")
Zanna.IO.File.Move(copyfile, movefile)
PRINT "moved exists: "; Zanna.IO.File.Exists(movefile)
PRINT "old gone: "; (NOT Zanna.IO.File.Exists(copyfile))

' --- File: delete ---
Zanna.IO.File.Delete(movefile)
PRINT "deleted: "; (NOT Zanna.IO.File.Exists(movefile))

' --- File: touch ---
DIM touchfile AS STRING
touchfile = Zanna.IO.TempFile.PathWithExt("vtest_touch", ".txt")
Zanna.IO.File.Touch(touchfile)
PRINT "touched exists: "; Zanna.IO.File.Exists(touchfile)
Zanna.IO.File.Delete(touchfile)

' --- File: modified ---
DIM modtime AS LONG
modtime = Zanna.IO.File.Modified(testfile)
PRINT "modified > 0: "; (modtime > 0)

' --- Dir: make, exists, remove ---
DIM testdir AS STRING
testdir = Zanna.IO.TempFile.PathWithPrefix("vtest_dir")
Zanna.IO.Dir.Make(testdir)
PRINT "dir exists: "; Zanna.IO.Dir.Exists(testdir)
Zanna.IO.Dir.Remove(testdir)
PRINT "dir removed: "; (NOT Zanna.IO.Dir.Exists(testdir))

' --- Dir: makeall ---
DIM deepdir AS STRING
deepdir = Zanna.IO.TempFile.PathWithPrefix("vtest_deep")
DIM subdir AS STRING
subdir = Zanna.IO.Path.Join(deepdir, "a")
subdir = Zanna.IO.Path.Join(subdir, "b")
Zanna.IO.Dir.MakeAll(subdir)
PRINT "deep dir exists: "; Zanna.IO.Dir.Exists(subdir)
Zanna.IO.Dir.RemoveAll(deepdir)
PRINT "deep dir removed: "; (NOT Zanna.IO.Dir.Exists(deepdir))

' --- Dir: current ---
DIM curdir AS STRING
curdir = Zanna.IO.Dir.Current()
PRINT "curdir nonempty: "; (LEN(curdir) > 0)

' --- IO.Glob: match ---
PRINT "glob match: "; Zanna.IO.Glob.Match("hello.txt", "*.txt")
PRINT "glob nomatch: "; Zanna.IO.Glob.Match("hello.txt", "*.bas")

' --- Cleanup ---
Zanna.IO.File.Delete(testfile)

PRINT "done"
END
