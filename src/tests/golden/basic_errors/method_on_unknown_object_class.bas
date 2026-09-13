REM Calling a method on an OBJECT whose class is unknown is an error, not a silently
REM dropped call.
DIM list AS Zanna.Collections.List
list = NEW Zanna.Collections.List()
list.Push("alpha")
DIM o AS OBJECT
o = list.Get(0)
PRINT o.Concat("x")
END
