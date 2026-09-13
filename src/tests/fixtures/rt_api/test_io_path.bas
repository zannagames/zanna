' test_io_path.bas — Zanna.IO.Path + Zanna.IO.File + Zanna.IO.Dir
PRINT Zanna.IO.Path.Join("a", "b")
PRINT Zanna.IO.Path.Extension("file.txt")
PRINT Zanna.IO.Path.Name("dir/file.txt")
PRINT Zanna.IO.Path.Stem("dir/file.txt")
PRINT Zanna.IO.Path.Directory("dir/file.txt")
PRINT (Zanna.IO.Path.Extension("file.txt") <> "")
PRINT (Zanna.IO.Path.Extension("file") <> "")
PRINT Zanna.IO.Path.WithExtension("file.txt", ".md")
PRINT Zanna.IO.Path.IsAbsolute("C:\foo")
PRINT Zanna.IO.Path.IsAbsolute("foo")
PRINT Zanna.IO.Path.Normalize("a/b/../c")

' File write/read round-trip
DIM tmp AS STRING
LET tmp = Zanna.IO.TempFile.PathWithExt("rt_api_basic_io_path", ".txt")
Zanna.IO.File.WriteAllText(tmp, "hello from zanna")
PRINT Zanna.IO.File.Exists(tmp)
PRINT Zanna.IO.File.ReadAllText(tmp)
PRINT Zanna.IO.File.SizeBytes(tmp)
Zanna.IO.File.Append(tmp, "!")
PRINT Zanna.IO.File.ReadAllText(tmp)
Zanna.IO.File.Delete(tmp)
PRINT Zanna.IO.File.Exists(tmp)

' Dir
PRINT Zanna.IO.Dir.Exists(".")
PRINT "done"
END
