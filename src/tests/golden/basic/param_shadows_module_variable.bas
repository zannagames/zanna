REM A parameter names its own value even when a module-level variable has the same name:
REM in a SUB, a FUNCTION and a class constructor. A local DIM shadows the module variable too.
DIM r AS INTEGER
r = 3

SUB Show(r AS DOUBLE)
    PRINT "sub param: "; r
END SUB

FUNCTION Twice(r AS INTEGER) AS INTEGER
    RETURN r * 2
END FUNCTION

SUB Bump()
    DIM r AS INTEGER
    r = 99
    PRINT "local dim: "; r
END SUB

CLASS Circle
    PUBLIC radius AS DOUBLE
    PUBLIC SUB NEW(r AS DOUBLE)
        radius = r
    END SUB
END CLASS

Show(5.5)
PRINT "function param: "; Twice(21)
Bump()
DIM c AS Circle
c = NEW Circle(2.5)
PRINT "ctor param: "; c.radius
PRINT "module variable: "; r
END
