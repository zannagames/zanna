REM Example: Mixed usage with List + String + (optional) File I/O

DIM list AS Zanna.Collections.List
list = NEW Zanna.Collections.List()

list.Push("alpha")
list.Push("beta")

DIM joined AS STRING
joined = Zanna.Core.Box.ToStr(list.Get(0)) + "-" + Zanna.Core.Box.ToStr(list.Get(1))
PRINT "report: "; joined

REM Optional file roundtrip (enable if your build exposes Zanna.IO.File.*)
'Zanna.IO.File.WriteAllText("oop_mixed_report.tmp", joined)
'PRINT Zanna.IO.File.ReadAllText("oop_mixed_report.tmp")
'Zanna.IO.File.Delete("oop_mixed_report.tmp")
