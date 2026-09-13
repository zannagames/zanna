REM STR$ formats by the value's type: integers at their full 64-bit width, and float
REM fields, float FUNCTION and method results, float runtime results and float array
REM elements with their fractions.
CLASS Pt
    PUBLIC x AS DOUBLE
    PUBLIC ws(2) AS DOUBLE
    PUBLIC FUNCTION Half() AS DOUBLE
        Half = x / 2
    END FUNCTION
    PUBLIC FUNCTION Third() AS STRING
        ws(1) = x / 3
        Third = STR$(ws(1))
    END FUNCTION
END CLASS
FUNCTION TWICE#(N#)
    TWICE# = N# * 2
END FUNCTION
DIM big AS INTEGER
big = 5000000000
PRINT STR$(big); " "; STR$(-big); " "; STR$(1 = 1)
DIM p AS Pt
p = NEW Pt()
p.x = 7.5
PRINT STR$(p.x); " "; STR$(p.Half()); " "; p.Third()
PRINT STR$(TWICE#(1.25)); " "; STR$(Zanna.Math.Sqrt(2.25))
DIM w(2) AS DOUBLE
w(2) = 0.125
PRINT STR$(w(2)); " "; STR$(p.ws(1))
END
