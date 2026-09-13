REM A method call on a variable declared with an interface type dispatches through
REM the interface to the implementing class.
INTERFACE I
  SUB Speak()
  FUNCTION Legs() AS INTEGER
END INTERFACE

CLASS A IMPLEMENTS I
  SUB Speak()
    PRINT "A"
  END SUB
  FUNCTION Legs() AS INTEGER
    RETURN 4
  END FUNCTION
END CLASS

DIM x AS I
LET x = NEW A()
x.Speak()
PRINT x.Legs()
END
