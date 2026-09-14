REM HEX$, OCT$, SPACE$, and STRING$ build strings from numbers and characters.
DIM n AS INTEGER
DIM d AS DOUBLE
n = 255
d = 26.5
PRINT HEX$(n); " "; HEX$(0); " "; HEX$(-1); " "; HEX$(d); " "; HEX$(4096)
PRINT OCT$(8); " "; OCT$(n); " "; OCT$(-1)
PRINT "["; SPACE$(3); "]"; LEN(SPACE$(0))
PRINT STRING$(4, "xyz"); " "; STRING$(3, 65); " "; STRING$(n \ 85, "-"); "|"; STRING$(0, "q"); "|"
DIM s AS STRING
s = "(" + SPACE$(2) + STRING$(2, 42) + HEX$(171) + ")"
PRINT s
IF HEX$(10) = "A" THEN PRINT "hex compares"
SELECT CASE STRING$(2, "a")
  CASE "aa"
    PRINT "select on STRING$"
END SELECT
FUNCTION Pad(t AS STRING, w AS INTEGER) AS STRING
  RETURN t + SPACE$(w - LEN(t))
END FUNCTION
PRINT "["; Pad("ab", 5); "]"
