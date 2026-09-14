REM A variable declared AS a class takes objects of that class, of a subclass, and of a
REM class implementing a declared interface; an OBJECT variable takes any object.
INTERFACE Shape
  FUNCTION Area() AS INTEGER
END INTERFACE

CLASS Animal
  PUBLIC name AS STRING
END CLASS

CLASS Dog : Animal
END CLASS

CLASS Square IMPLEMENTS Shape
  PUBLIC side AS INTEGER
  FUNCTION Area() AS INTEGER
    RETURN side * side
  END FUNCTION
END CLASS

DIM pet AS Animal
pet = NEW Dog()
pet.name = "rex"
PRINT pet.name

DIM sq AS Square
sq = NEW Square()
sq.side = 3
DIM s AS Shape
s = sq
PRINT s.Area()

DIM anything AS OBJECT
anything = Zanna.Collections.Map.New()
anything = sq
PRINT "ok"
END
