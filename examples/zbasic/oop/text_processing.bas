REM Example: Text processing using StringBuilder and File static methods
REM Note: Some BASIC keywords (APPEND, DELETE) may conflict with member names.
REM If your build treats these as reserved, use the procedural helpers as shown.

REM Variant A: Object members (may collide with keywords)
'USING Zanna.IO
'
' Canonical namespace example:
'DIM sb AS Zanna.Text.StringBuilder
'sb = NEW Zanna.Text.StringBuilder()
'
'sb.Append("hello")
'sb.Append(" ")
'sb.Append("world")
'sb.Append("!")
'
'DIM content AS STRING
'content = sb.ToString()
'
'File.WriteAllText("oop_text_proc.tmp", content)
'PRINT File.ReadAllText("oop_text_proc.tmp")
'File.Delete("oop_text_proc.tmp")

REM Variant B: Procedural helpers (portable across builds)
DIM sb AS Zanna.Text.StringBuilder
sb = NEW Zanna.Text.StringBuilder()
sb = Zanna.Text.StringBuilder.Append(sb, "hello")
sb = Zanna.Text.StringBuilder.Append(sb, " ")
sb = Zanna.Text.StringBuilder.Append(sb, "world")
sb = Zanna.Text.StringBuilder.Append(sb, "!")
PRINT sb.ToString()

REM Use Zanna.IO.File.* helpers when available in your build
'Zanna.IO.File.WriteAllText("oop_text_proc.tmp", sb.ToString())
'PRINT Zanna.IO.File.ReadAllText("oop_text_proc.tmp")
'Zanna.IO.File.Delete("oop_text_proc.tmp")
