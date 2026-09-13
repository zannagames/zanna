' ===----------------------------------------------------------------------===
'
' Part of the Zanna project, under the GNU GPL v3.
' See LICENSE for license information.
'
' ===----------------------------------------------------------------------===
'
' File: src/tests/fixtures/runtime/test_basic_class_lifetimes.bas
' Purpose: Prove that BASIC class objects with array fields of every element
'          kind construct, read, and destroy correctly; that FUNCTIONs
'          declared AS <Class> type their results as that class at the call
'          site (implicit variables, member access on the call, NOTHING); and
'          that FUNCTION and method STRING/object results are owned by the
'          caller on every RETURN path.
' Key invariants:
'   - Destroying a holder releases each array field with the helper matching
'     the element kind its constructor allocated (string, integer, float,
'     boolean and object arrays), so an object stored only in a holder's
'     array dies with the holder and no destructor traps.
'   - A DOUBLE array field stores and reads floats; STR$ of a DOUBLE array
'     element keeps its fraction.
'   - A FUNCTION AS <Class> result assigned to an undeclared variable gives
'     that variable the class, and its members read through the call.
'   - RETURN of a field, parameter, local, NEW object or string hands the
'     caller exactly one reference: repeated calls never free what the callee
'     still owns, and a result dies once its last caller-side owner drops it.
' Ownership/Lifetime:
'   - Objects are owned by module variables, procedure locals, or holder
'     array fields; WeakRefs only observe.
' Links: src/frontends/basic/lower/oop/Lower_OOP_Emit.cpp,
'        src/frontends/basic/lower/oop/Lower_OOP_RuntimeHelpers.cpp
'
' ===----------------------------------------------------------------------===

CLASS Item
    PUBLIC label AS STRING
END CLASS

CLASS Holder
    PUBLIC title AS STRING
    PUBLIC names(2) AS STRING
    PUBLIC counts(2) AS INTEGER
    PUBLIC weights(2) AS DOUBLE
    PUBLIC flags(2) AS BOOLEAN
    PUBLIC items(2) AS Item

    PUBLIC SUB Fill(tag AS STRING)
        DIM i AS INTEGER
        title = "holder " + tag
        FOR i = 0 TO 2
            names(i) = tag + STR$(i)
            counts(i) = i * 10
            weights(i) = i + 0.5
            flags(i) = (i = 1)
        NEXT i
    END SUB

    PUBLIC FUNCTION Describe() AS STRING
        Describe = title + ": " + names(2) + " " + STR$(counts(2)) + " " + STR$(weights(2))
    END FUNCTION
END CLASS

CLASS Shelf
    PUBLIC held AS Item
    PUBLIC label AS STRING

    PUBLIC FUNCTION GetHeld() AS Item
        RETURN held
    END FUNCTION

    PUBLIC FUNCTION GetLabel() AS STRING
        RETURN label
    END FUNCTION

    PUBLIC FUNCTION Fresh() AS Item
        RETURN NEW Item()
    END FUNCTION

    PUBLIC FUNCTION LabelTwice() AS STRING
        RETURN GetLabel() + GetLabel()
    END FUNCTION
END CLASS

FUNCTION PassThrough(it AS Item) AS Item
    RETURN it
END FUNCTION

FUNCTION Build(tag AS STRING) AS Item
    DIM made AS Item
    made = NEW Item()
    made.label = tag
    RETURN made
END FUNCTION

FUNCTION Echo$(s$)
    RETURN s$
END FUNCTION

FUNCTION MakeHolder(tag AS STRING) AS Holder
    DIM h AS Holder
    h = NEW Holder()
    h.Fill(tag)
    MakeHolder = h
END FUNCTION

FUNCTION MaybeHolder(tag AS STRING) AS Holder
    IF tag = "" THEN
        MaybeHolder = NOTHING
        EXIT FUNCTION
    END IF
    MaybeHolder = MakeHolder(tag)
END FUNCTION

DIM fails AS INTEGER
fails = 0

SUB Check(ok AS BOOLEAN, what AS STRING)
    IF NOT ok THEN
        PRINT "FAIL: "; what
        fails = fails + 1
    END IF
END SUB

' Parks an object in a procedure-local holder that dies when the SUB returns.
SUB ParkInLocalHolder(it AS Item)
    DIM h AS Holder
    h = NEW Holder()
    h.Fill("local")
    h.items(1) = it
END SUB

' 1. Every array field kind reads back what the methods stored.
DIM held AS Holder
held = MakeHolder("a")
Check(held.Describe() = "holder a: a2 20 2.5", "array field reads: " + held.Describe())

' 2. Replacing and clearing holders destroys them without trapping, and an object
'    stored only in a destroyed holder's array field dies with it.
DIM inner AS Item
DIM innerRef AS OBJECT
DIM box AS Holder
DIM round AS INTEGER
inner = NEW Item()
inner.label = "inner"
innerRef = Zanna.Memory.WeakRef.New(inner)
FOR round = 1 TO 3
    box = NEW Holder()
    box.Fill("round" + STR$(round))
    box.items(0) = inner
NEXT round
inner = NOTHING
Check(Zanna.Memory.WeakRef.IsAlive(innerRef), "item died while a holder still stores it")
box = NOTHING
Check(NOT Zanna.Memory.WeakRef.IsAlive(innerRef), "item survived the holder that stored it")

' 3. A procedure-local holder dies at the end of its SUB.
DIM parked AS Item
DIM parkedRef AS OBJECT
parked = NEW Item()
parkedRef = Zanna.Memory.WeakRef.New(parked)
ParkInLocalHolder(parked)
parked = NOTHING
Check(NOT Zanna.Memory.WeakRef.IsAlive(parkedRef), "item survived a procedure-local holder")

' 4. FUNCTION AS <Class> results carry their class to the call site.
implicitHolder = MakeHolder("b")
Check(implicitHolder.title = "holder b", "implicit variable class: " + implicitHolder.title)
Check(MakeHolder("c").title = "holder c", "member access on a call result")
Check(Zanna.Core.Object.RefEquals(MaybeHolder(""), NOTHING), "NOTHING FUNCTION result")
Check(MaybeHolder("d").counts(1) = 10, "array field through a relayed FUNCTION result")

' 5. DOUBLE array elements keep their fractions.
DIM w(2) AS DOUBLE
DIM k AS INTEGER
k = 2
w(k) = k + 0.25
Check(STR$(w(2)) = "2.25", "STR$ of a DOUBLE array element: " + STR$(w(2)))
Check(STR$(held.weights(1)) = "1.5", "STR$ of a DOUBLE array field element")

' 6. RETURN hands the caller one reference on every path.
DIM shelf AS Shelf
DIM shelfItem AS Item
DIM shelfItemRef AS OBJECT
DIM churn$
DIM seen$
shelf = NEW Shelf()
shelfItem = NEW Item()
shelfItem.label = "kept"
shelfItemRef = Zanna.Memory.WeakRef.New(shelfItem)
shelf.held = shelfItem
shelf.label = "shelf" + STR$(9)
FOR round = 1 TO 40
    seen$ = shelf.GetHeld().label + shelf.GetLabel() + shelf.LabelTwice()
    seen$ = seen$ + PassThrough(shelfItem).label + Echo$(shelf.label)
    churn$ = STR$(round) + "-" + seen$
NEXT round
Check(seen$ = "keptshelf9shelf9shelf9keptshelf9", "returned fields and parameters: " + seen$)
shelfItem = NOTHING
Check(Zanna.Memory.WeakRef.IsAlive(shelfItemRef), "returning a field released the shelf's item")
shelf = NOTHING
Check(NOT Zanna.Memory.WeakRef.IsAlive(shelfItemRef), "returned field outlived its shelf")

DIM built AS Item
DIM builtRef AS OBJECT
built = Build("made")
builtRef = Zanna.Memory.WeakRef.New(built)
Check(built.label = "made", "returned local: " + built.label)
built = NOTHING
Check(NOT Zanna.Memory.WeakRef.IsAlive(builtRef), "returned local outlived its caller-side owner")

DIM fresh AS Item
DIM freshRef AS OBJECT
shelf = NEW Shelf()
fresh = shelf.Fresh()
freshRef = Zanna.Memory.WeakRef.New(fresh)
fresh = NOTHING
Check(NOT Zanna.Memory.WeakRef.IsAlive(freshRef), "returned NEW object outlived its caller-side owner")

IF fails = 0 THEN
    PRINT "RESULT: ok"
ELSE
    PRINT "RESULT: "; fails; " failure(s)"
END IF
END
