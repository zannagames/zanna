' Zanna.Text.TextWrapper API Audit - Text Wrapping and Formatting
' Tests all TextWrapper functions

PRINT "=== Zanna.Text.TextWrapper API Audit ==="

DIM longText AS STRING
longText = "The quick brown fox jumps over the lazy dog. This is a longer sentence that should be wrapped at the specified width."

' --- Wrap ---
PRINT "--- Wrap ---"
PRINT Zanna.Text.TextWrapper.Wrap(longText, 40)

' --- WrapLines ---
PRINT "--- WrapLines ---"
DIM lines AS Zanna.Collections.Seq
lines = Zanna.Text.TextWrapper.WrapLines(longText, 40)
PRINT "Line count: "; lines.Count
' Note: Accessing lines.Get(N) crashes due to known heap corruption bug
' with interim objects after many function calls.

' --- Fill ---
PRINT "--- Fill ---"
PRINT Zanna.Text.TextWrapper.Fill(longText, 40)

' --- Indent ---
PRINT "--- Indent ---"
DIM txt AS STRING
txt = "line1" + CHR(10) + "line2" + CHR(10) + "line3"
PRINT Zanna.Text.TextWrapper.Indent(txt, "  ")
PRINT Zanna.Text.TextWrapper.Indent(txt, ">>> ")

' --- Dedent ---
PRINT "--- Dedent ---"
DIM indented AS STRING
indented = "    line1" + CHR(10) + "    line2" + CHR(10) + "    line3"
PRINT Zanna.Text.TextWrapper.Dedent(indented)

' --- Hang ---
PRINT "--- Hang ---"
DIM paragraph AS STRING
paragraph = "This is the first line of a paragraph that will have a hanging indent applied to it for demonstration."
PRINT Zanna.Text.TextWrapper.Hang(paragraph, "    ")

' --- Truncate ---
PRINT "--- Truncate ---"
PRINT Zanna.Text.TextWrapper.Truncate("Hello, World! This is a long string.", 20)
PRINT Zanna.Text.TextWrapper.Truncate("Short", 20)

' --- TruncateWith ---
PRINT "--- TruncateWith ---"
PRINT Zanna.Text.TextWrapper.TruncateWith("Hello, World! This is a long string.", 20, " [...]")
PRINT Zanna.Text.TextWrapper.TruncateWith("Short", 20, " [...]")

' --- Shorten ---
PRINT "--- Shorten ---"
PRINT Zanna.Text.TextWrapper.Shorten("Hello, World! This is a long string.", 20)

' --- Left / Right / Center ---
PRINT "--- Left / Right / Center ---"
PRINT "["; Zanna.Text.TextWrapper.AlignLeft("hello", 20); "]"
PRINT "["; Zanna.Text.TextWrapper.AlignRight("hello", 20); "]"
PRINT "["; Zanna.Text.TextWrapper.Center("hello", 20); "]"

' --- LineCount ---
PRINT "--- LineCount ---"
PRINT "3 lines: "; Zanna.Text.TextWrapper.LineCount("one" + CHR(10) + "two" + CHR(10) + "three")
PRINT "1 line: "; Zanna.Text.TextWrapper.LineCount("single")
PRINT "empty: "; Zanna.Text.TextWrapper.LineCount("")

' --- MaxLineLen ---
PRINT "--- MaxLineLen ---"
PRINT "Max len: "; Zanna.Text.TextWrapper.MaxLineLength("short" + CHR(10) + "a much longer line" + CHR(10) + "med")
PRINT "Single: "; Zanna.Text.TextWrapper.MaxLineLength("single")

PRINT "=== TextWrapper Demo Complete ==="
END
