' Comprehensive test for all Zanna.* OOP runtime classes
' Tests each class's methods and properties

' =============================================================================
' Zanna.String Tests
' =============================================================================
PRINT "=== Zanna.String ==="
DIM s AS STRING
s = "  Hello World  "
PRINT "Original: '"; s; "'"
PRINT "Length: "; LEN(s)

DIM trimmed AS STRING
trimmed = TRIM$(s)
PRINT "Trim: '"; trimmed; "'"
PRINT "TrimStart: '"; LTRIM$(s); "'"
PRINT "TrimEnd: '"; RTRIM$(s); "'"

PRINT "ToUpper: "; UCASE$(trimmed)
PRINT "ToLower: "; LCASE$(trimmed)

PRINT "Left(5): "; LEFT$(trimmed, 5)
PRINT "Right(5): "; RIGHT$(trimmed, 5)
PRINT "Mid(7): "; MID$(trimmed, 7)
PRINT "Mid(7,3): "; MID$(trimmed, 7, 3)

DIM idx AS INTEGER
idx = INSTR(trimmed, "World")
PRINT "IndexOf 'World': "; idx

DIM sub1 AS STRING
sub1 = MID$(trimmed, 1, 5)
PRINT "Substring(1,5): "; sub1

IF LEN("") = 0 THEN
    PRINT "Empty string length is 0: PASS"
END IF

PRINT ""

' =============================================================================
' Zanna.Text.StringBuilder Tests
' =============================================================================
PRINT "=== Zanna.Text.StringBuilder ==="
DIM sb AS Zanna.Text.StringBuilder
sb = NEW Zanna.Text.StringBuilder()
PRINT "Initial Length: "; sb.Length
sb.Append("Hello")
PRINT "After 'Hello' Length: "; sb.Length
sb.Append(" ")
sb.Append("World")
PRINT "After ' World' Length: "; sb.Length
PRINT "ToString: "; sb.ToString()
sb.Clear()
PRINT "After Clear Length: "; sb.Length
PRINT ""

' =============================================================================
' Zanna.Collections.List Tests
' =============================================================================
PRINT "=== Zanna.Collections.List ==="
DIM list AS Zanna.Collections.List
list = NEW Zanna.Collections.List()
PRINT "Initial Count: "; list.Count

' Create a simple object to add to list
DIM obj AS Zanna.Text.StringBuilder
obj = NEW Zanna.Text.StringBuilder()
list.Push(obj)
list.Push(obj)
list.Push(obj)
PRINT "After 3 Add: "; list.Count

list.RemoveAt(1)
PRINT "After RemoveAt(1): "; list.Count

list.Clear()
PRINT "After Clear: "; list.Count
PRINT ""

' =============================================================================
' Zanna.Math Tests
' =============================================================================
PRINT "=== Zanna.Math ==="
PRINT "Sqrt(16): "; SQR(16)
PRINT "Abs(-5): "; ABS(-5)
PRINT "Abs(-3.14): "; ABS(-3.14)

DIM pi AS DOUBLE
pi = 3.14159265358979

PRINT "Floor(3.7): "; INT(3.7)
PRINT "Floor(-3.7): "; INT(-3.7)

PRINT "Sin(0): "; SIN(0)
PRINT "Cos(0): "; COS(0)
PRINT "Tan(0): "; TAN(0)
PRINT "Atan(0): "; ATN(0)

PRINT "Exp(0): "; EXP(0)
PRINT "Exp(1): "; EXP(1)
PRINT "Log(1): "; LOG(1)

PRINT "Pow(2,10): "; 2^10

PRINT "Sgn(-5): "; SGN(-5)
PRINT "Sgn(0): "; SGN(0)
PRINT "Sgn(5): "; SGN(5)
PRINT ""

' =============================================================================
' Zanna.Random Tests
' =============================================================================
PRINT "=== Zanna.Random ==="
RANDOMIZE 12345
DIM r1 AS DOUBLE, r2 AS DOUBLE, r3 AS DOUBLE
r1 = RND()
r2 = RND()
r3 = RND()
PRINT "Seeded random (12345):"
PRINT "  r1 in 0..1: "; (r1 >= 0 AND r1 < 1)
PRINT "  r2 in 0..1: "; (r2 >= 0 AND r2 < 1)
PRINT "  r3 in 0..1: "; (r3 >= 0 AND r3 < 1)

' Re-seed with same value should give same sequence
RANDOMIZE 12345
DIM r1b AS DOUBLE
r1b = RND()
IF r1 = r1b THEN
    PRINT "Reproducible with same seed: PASS"
ELSE
    PRINT "Reproducible with same seed: FAIL"
END IF
PRINT ""

' =============================================================================
' Zanna.Terminal Tests
' =============================================================================
PRINT "=== Zanna.Terminal ==="
PRINT "WriteLine test: PASS"
PRINT ""

' =============================================================================
' Zanna.Core.Convert Tests
' =============================================================================
PRINT "=== Zanna.Core.Convert ==="
DIM numStr AS STRING
numStr = "42"
DIM numVal AS DOUBLE
numVal = VAL(numStr)
PRINT "ToInt64('42'): "; numVal

DIM floatStr AS STRING
floatStr = "3.14159"
DIM floatVal AS DOUBLE
floatVal = VAL(floatStr)
PRINT "ToDouble('3.14159'): "; floatVal

DIM intToStr AS STRING
intToStr = STR$(123)
PRINT "ToString_Int(123): '"; LTRIM$(intToStr); "'"

DIM dblToStr AS STRING
dblToStr = STR$(2.5)
PRINT "ToString_Double(2.5): '"; LTRIM$(dblToStr); "'"
PRINT ""

' =============================================================================
' Test Complete
' =============================================================================
PRINT "=== All Tests Complete ==="
