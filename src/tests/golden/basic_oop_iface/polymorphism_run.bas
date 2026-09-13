' Test: OOP - Interfaces
' Tests: INTERFACE, IMPLEMENTS

' Define interface
INTERFACE IShape
    FUNCTION GetArea() AS DOUBLE
    FUNCTION GetName$() AS STRING
END INTERFACE

' Implement interface in Circle
CLASS Circle IMPLEMENTS IShape
    PRIVATE radius AS DOUBLE

    PUBLIC SUB NEW(r AS DOUBLE)
        radius = r
    END SUB

    PUBLIC FUNCTION GetArea() AS DOUBLE
        GetArea = 3.14159 * radius * radius
    END FUNCTION

    PUBLIC FUNCTION GetName$() AS STRING
        GetName$ = "Circle"
    END FUNCTION

    PUBLIC FUNCTION GetRadius() AS DOUBLE
        GetRadius = radius
    END FUNCTION
END CLASS

' Implement interface in Rectangle
CLASS Rectangle IMPLEMENTS IShape
    PRIVATE width AS DOUBLE
    PRIVATE height AS DOUBLE

    PUBLIC SUB NEW(w AS DOUBLE, h AS DOUBLE)
        width = w
        height = h
    END SUB

    PUBLIC FUNCTION GetArea() AS DOUBLE
        GetArea = width * height
    END FUNCTION

    PUBLIC FUNCTION GetName$() AS STRING
        GetName$ = "Rectangle"
    END FUNCTION
END CLASS

' Main program
PRINT "=== OOP Interfaces Test ==="

' Test Circle
PRINT ""
PRINT "--- Circle ---"
DIM c AS Circle
c = NEW Circle(5.0)
PRINT "Shape: "; c.GetName$()
PRINT "Area: "; c.GetArea()
PRINT "Radius: "; c.GetRadius()

' Test Rectangle
PRINT ""
PRINT "--- Rectangle ---"
DIM r AS Rectangle
r = NEW Rectangle(4.0, 6.0)
PRINT "Shape: "; r.GetName$()
PRINT "Area: "; r.GetArea()

' Test polymorphism through interface
PRINT ""
PRINT "--- Polymorphism via Interface ---"
DIM shape AS IShape

shape = c
PRINT "As IShape (circle): "; shape.GetName$(); " area = "; shape.GetArea()

shape = r
PRINT "As IShape (rect): "; shape.GetName$(); " area = "; shape.GetArea()

PRINT ""
PRINT "=== OOP Interfaces test complete ==="
