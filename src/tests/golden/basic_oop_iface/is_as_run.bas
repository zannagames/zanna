REM IS and AS on a class that implements an interface, and a call through the
REM interface-typed variable AS produces.
CLASS B
END CLASS

INTERFACE J
  SUB F()
END INTERFACE

CLASS C IMPLEMENTS J
  SUB F()
    PRINT 7
  END SUB
END CLASS

DIM o AS OBJECT
LET o = NEW C()
PRINT o IS C
PRINT o IS J
PRINT o IS B
DIM j AS J
LET j = o AS J
IF j IS J THEN j.F()
END
