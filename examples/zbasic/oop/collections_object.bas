REM Example: Collections + Object methods

NAMESPACE App
  CLASS Person
    SUB New()
    END SUB
  END CLASS
END NAMESPACE

DIM list AS Zanna.Collections.List
list = NEW Zanna.Collections.List()

DIM p1 AS App.Person
DIM p2 AS App.Person
p1 = NEW App.Person()
p2 = NEW App.Person()

list.Push(p1)
list.Push(p2)

PRINT list.Count
PRINT list.Get(0).ToString()
PRINT list.Get(1).Equals(p2)
PRINT list.Get(0).Equals(p2)
