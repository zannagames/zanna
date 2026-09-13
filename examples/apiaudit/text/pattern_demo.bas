' Zanna.Text.Pattern API Audit - Regex Pattern Matching
' Tests all Pattern functions
' Note: Pattern functions take (text, pattern) argument order

PRINT "=== Zanna.Text.Pattern API Audit ==="

' --- IsMatch ---
PRINT "--- IsMatch ---"
PRINT "IsMatch digits in 'abc123': "; Zanna.Text.Pattern.IsMatch("abc123", "[0-9]+")
PRINT "IsMatch digits in 'hello': "; Zanna.Text.Pattern.IsMatch("hello", "[0-9]+")
PRINT "IsMatch lowercase 'hello': "; Zanna.Text.Pattern.IsMatch("hello", "^[a-z]+$")
PRINT "IsMatch lowercase 'Hello': "; Zanna.Text.Pattern.IsMatch("Hello", "^[a-z]+$")

' --- Find ---
PRINT "--- Find ---"
PRINT "Find word: "; Zanna.Text.Pattern.Find("Hello World", "[A-Z][a-z]+").UnwrapStr()
DIM found AS OBJECT
found = Zanna.Text.Pattern.Find("abc123def", "[0-9]+")
PRINT "Find IsSome: "; found.IsSome
PRINT "Find value: "; found.UnwrapStr()
PRINT "Find no match: "; Zanna.Text.Pattern.Find("abcdef", "[0-9]+").IsNone

' --- FindFrom ---
PRINT "--- FindFrom ---"
DIM foundFrom AS OBJECT
foundFrom = Zanna.Text.Pattern.FindFrom("abc123def456", "[0-9]+", 6)
PRINT "FindFrom IsSome: "; foundFrom.IsSome
PRINT "FindFrom value: "; foundFrom.UnwrapStr()

' --- FindPos ---
PRINT "--- FindPos ---"
PRINT "FindPos digits: "; Zanna.Text.Pattern.FindPos("abc123def", "[0-9]+").UnwrapOrI64(-1)
PRINT "FindPos no match: "; Zanna.Text.Pattern.FindPos("abcdef", "[0-9]+").UnwrapOrI64(-1)
DIM foundPos AS OBJECT
foundPos = Zanna.Text.Pattern.FindPos("abc123def", "[0-9]+")
PRINT "FindPos IsSome: "; foundPos.IsSome
PRINT "FindPos value: "; foundPos.UnwrapI64()
PRINT "FindPos no match IsNone: "; Zanna.Text.Pattern.FindPos("abcdef", "[0-9]+").IsNone

' --- FindAll ---
PRINT "--- FindAll ---"
DIM matches AS Zanna.Collections.Seq
matches = Zanna.Text.Pattern.FindAll("a1b22c333", "[0-9]+")
PRINT "Match count: "; matches.Count
IF matches.Count > 0 THEN
    DIM m0 AS OBJECT
    m0 = matches.Get(0)
    PRINT "Match 0: "; m0
END IF
IF matches.Count > 1 THEN
    DIM m1 AS OBJECT
    m1 = matches.Get(1)
    PRINT "Match 1: "; m1
END IF
IF matches.Count > 2 THEN
    DIM m2 AS OBJECT
    m2 = matches.Get(2)
    PRINT "Match 2: "; m2
END IF

' --- Replace ---
PRINT "--- Replace ---"
PRINT Zanna.Text.Pattern.Replace("abc123def456", "[0-9]+", "NUM")
PRINT Zanna.Text.Pattern.Replace("hello   world   foo", "\s+", " ")

' --- ReplaceFirst ---
PRINT "--- ReplaceFirst ---"
PRINT Zanna.Text.Pattern.ReplaceFirst("abc123def456", "[0-9]+", "NUM")

' --- Split ---
PRINT "--- Split ---"
DIM parts AS Zanna.Collections.Seq
parts = Zanna.Text.Pattern.Split("one,two;;three,four", "[,;]+")
PRINT "Part count: "; parts.Count
IF parts.Count > 0 THEN
    DIM p0 AS OBJECT
    p0 = parts.Get(0)
    PRINT "Part 0: "; p0
END IF
IF parts.Count > 1 THEN
    DIM p1 AS OBJECT
    p1 = parts.Get(1)
    PRINT "Part 1: "; p1
END IF
IF parts.Count > 2 THEN
    DIM p2 AS OBJECT
    p2 = parts.Get(2)
    PRINT "Part 2: "; p2
END IF
IF parts.Count > 3 THEN
    DIM p3 AS OBJECT
    p3 = parts.Get(3)
    PRINT "Part 3: "; p3
END IF

' --- Escape ---
PRINT "--- Escape ---"
PRINT Zanna.Text.Pattern.Escape("hello.world+foo")
PRINT Zanna.Text.Pattern.Escape("[test](value)")

PRINT "=== Pattern Demo Complete ==="
END
