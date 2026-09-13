REM Mixed usage: String + List + File + StringBuilder (procedural helpers to avoid keyword collisions)

DIM s AS STRING
DIM sb AS Zanna.Text.StringBuilder
sb = NEW Zanna.Text.StringBuilder()

REM Seed base text
s = "alpha beta"

REM Split into two parts using Substring (positions are deterministic)
DIM s1 AS STRING
DIM s2 AS STRING
s1 = s.Substring(0, 5)
s2 = s.Substring(6, 4)

DIM list AS Zanna.Collections.List
list = NEW Zanna.Collections.List()
list.Push(s1)
list.Push(s2)

PRINT list.Count
PRINT list.Get(0)
PRINT list.Get(1)

REM Build a simple report string using Concat and StringBuilder
DIM joined AS STRING
joined = Zanna.Core.Object.ToString(list.Get(0)) + "-" + Zanna.Core.Object.ToString(list.Get(1))

sb = Zanna.Text.StringBuilder.Append(sb, "report:")
sb = Zanna.Text.StringBuilder.Append(sb, " ")
sb = Zanna.Text.StringBuilder.Append(sb, joined)

PRINT sb.ToString()

Zanna.IO.File.WriteAllText("oop_mixed_report.tmp", sb.ToString())
PRINT Zanna.IO.File.Exists("oop_mixed_report.tmp")
PRINT Zanna.IO.File.ReadAllText("oop_mixed_report.tmp")
Zanna.IO.File.Delete("oop_mixed_report.tmp")
PRINT Zanna.IO.File.Exists("oop_mixed_report.tmp")
