REM Classes declared inside a NAMESPACE are real objects: NEW allocates their fields, a class
REM without SUB NEW gets a default constructor, a derived class inherits its base's fields, and
REM virtual calls dispatch through a base-typed variable.
NAMESPACE Zoo
  CLASS Animal
    PUBLIC name AS STRING
    PUBLIC legs AS INTEGER
    SUB NEW(n AS STRING, l AS INTEGER)
      name = n
      legs = l
    END SUB
    VIRTUAL FUNCTION Describe() AS STRING
      RETURN name + " has " + STR$(legs) + " legs"
    END FUNCTION
  END CLASS
  CLASS Bird : Animal
    PUBLIC canFly AS BOOLEAN
    SUB NEW(n AS STRING)
      name = n
      legs = 2
      canFly = TRUE
    END SUB
    OVERRIDE FUNCTION Describe() AS STRING
      RETURN name + " flies"
    END FUNCTION
  END CLASS
  CLASS Tag
    PUBLIC label AS STRING
  END CLASS
END NAMESPACE

DIM a AS Zoo.Animal
a = NEW Zoo.Animal("Cat", 4)
PRINT a.Describe()
DIM b AS Zoo.Bird
b = NEW Zoo.Bird("Robin")
PRINT b.Describe()
PRINT b.legs
DIM c AS Zoo.Animal
c = b
PRINT c.Describe()
DIM t AS Zoo.Tag
t = NEW Zoo.Tag()
t.label = "sparrow"
PRINT t.label
PRINT t.ToString()
END
