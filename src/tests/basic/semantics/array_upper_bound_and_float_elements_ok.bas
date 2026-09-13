' Writes at an array's inclusive upper bound are in range, and float arrays take float
' elements without narrowing (no diagnostics expected).

DIM V(3) AS INTEGER
DIM F(2) AS DOUBLE
DIM H#(1)
DIM S$(2)

LET V(3) = 7
LET F(2) = 2.5
LET H#(1) = 0.25
LET S$(2) = "top"
LET F(0) = V(3)
PRINT STR$(F(2)); " "; H#(1) * 2; " "; S$(2)

END
