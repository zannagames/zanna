REM Static fields are module storage shared by every instance; static methods are called
REM through the class name; the static constructors run before the program and the static
REM destructors when it ends, both in class declaration order.
CLASS Counter
  STATIC total AS INTEGER
  STATIC label AS STRING

  STATIC SUB NEW()
    PRINT "Counter static constructor"
    total = 10
    Counter.label = "ready"
  END SUB

  STATIC DESTRUCTOR
    PRINT "Counter static destructor"; total
  END DESTRUCTOR

  SUB Bump()
    total = total + 1
  END SUB

  STATIC FUNCTION Twice() AS INTEGER
    RETURN total * 2
  END FUNCTION

  STATIC SUB Reset(value AS INTEGER)
    Counter.total = value
  END SUB
END CLASS

CLASS Log
  STATIC SUB NEW()
    PRINT "Log static constructor"
  END SUB

  STATIC DESTRUCTOR
    PRINT "Log static destructor"
  END DESTRUCTOR
END CLASS

SUB Finish()
  PRINT "finishing"
  END
END SUB

PRINT "main starts: "; Counter.label; Counter.total
DIM a AS Counter
DIM b AS Counter
a = NEW Counter()
b = NEW Counter()
a.Bump()
b.Bump()
PRINT "after bumps"; Counter.total; a.total; b.total
PRINT "twice"; Counter.Twice()
Counter.Reset(3)
b.total = b.total + 4
PRINT "after reset"; a.total
Finish()
PRINT "not reached"
