' ===----------------------------------------------------------------------===
'
' Part of the Zanna project, under the GNU GPL v3.
' See LICENSE for license information.
'
' ===----------------------------------------------------------------------===
'
' File: src/tests/fixtures/runtime/test_basic_managed_ownership.bas
' Purpose: Prove the BASIC managed-value convention (ADR 0147): every STRING and
'          object slot owns exactly one reference, temporaries die at the end of
'          their statement, and procedures own their parameters and locals.
' Key invariants:
'   - A NEW result passed straight to a call, or stored in an array element,
'     lives only as long as its owner.
'   - Reading an array element, binding it in FOR EACH, or calling a method on it
'     adds no lasting reference.
'   - Assigning to a STRING or object parameter never releases the caller's
'     value; what the procedure assigned dies when it returns.
'   - RETURN, EXIT, and a bare RETURN in a SUB release the procedure's locals.
'   - USING over an existing object leaves that object alive, and the USING
'     variable names the resource inside the body.
'   - A CATCH variable holds the handled error's code.
'   - A STRING is never null: module variables, STATIC locals, array elements,
'     and instance and static fields all start as "".
' Ownership/Lifetime:
'   - Objects are owned by module variables, locals, and array elements; WeakRefs
'     only observe.
' Links: docs/adr/0147-managed-reference-lowering-and-native-retain-elision.md,
'        src/frontends/basic/RuntimeStatementLowerer_Assign.cpp
'
' ===----------------------------------------------------------------------===

CLASS Item
    PUBLIC label AS STRING

    PUBLIC FUNCTION Tag() AS STRING
        Tag = "<" + label + ">"
    END FUNCTION

    PUBLIC SUB Rename(newLabel AS STRING)
        newLabel = newLabel + "!"
        label = newLabel
    END SUB
END CLASS

CLASS Shelf
    PUBLIC title AS STRING
    PUBLIC labels(1) AS STRING
    STATIC motto AS STRING
END CLASS

DIM fails AS INTEGER
DIM lastRef AS OBJECT
DIM sharedText AS STRING

FUNCTION GlobalIsEmpty() AS BOOLEAN
    GlobalIsEmpty = sharedText = ""
END FUNCTION

' True on the first call only: the STATIC local starts as "" and keeps what it is given.
FUNCTION StaticIsEmpty() AS BOOLEAN
    STATIC memo AS STRING
    StaticIsEmpty = memo = ""
    memo = memo + "seen"
END FUNCTION

SUB Check(ok AS BOOLEAN, what AS STRING)
    IF NOT ok THEN
        PRINT "FAIL: "; what
        fails = fails + 1
    END IF
END SUB

SUB Remember(it AS Item)
    lastRef = Zanna.Memory.WeakRef.New(it)
END SUB

SUB Replace(it AS Item)
    it = NEW Item()
    it.label = "replacement"
    lastRef = Zanna.Memory.WeakRef.New(it)
END SUB

SUB Suffix(text AS STRING)
    text = text + "-changed"
END SUB

SUB EarlyOut(leave AS BOOLEAN)
    DIM local AS Item
    local = NEW Item()
    lastRef = Zanna.Memory.WeakRef.New(local)
    IF leave THEN RETURN
    local.label = "late"
END SUB

FUNCTION Greeting(who AS STRING) AS STRING
    DIM text AS STRING
    text = "hi " + who
    who = "reassigned"
    Greeting = text
END FUNCTION

FUNCTION FirstLabel(it AS Item) AS STRING
    DIM keep AS Item
    keep = it
    IF keep.label = "" THEN
        FirstLabel = "empty"
        EXIT FUNCTION
    END IF
    RETURN keep.label + "/" + it.Tag()
END FUNCTION

' 1. A NEW result passed straight to a call dies after the statement.
Remember(NEW Item())
Check(NOT Zanna.Memory.WeakRef.IsAlive(lastRef), "NEW argument outlived its call")

' 2. An array element owns its object; reading it adds no lasting reference.
DIM items(2) AS Item
DIM copy AS Item
DIM elemRef AS OBJECT
DIM seen AS STRING
items(0) = NEW Item()
items(0).label = "zero"
elemRef = Zanna.Memory.WeakRef.New(items(0))
copy = items(0)
seen = items(0).label + items(0).Tag() + copy.Tag()
Check(seen = "zero<zero><zero>", "element reads: " + seen)
items(0) = NOTHING
Check(Zanna.Memory.WeakRef.IsAlive(elemRef), "copy lost its element")
copy = NOTHING
Check(NOT Zanna.Memory.WeakRef.IsAlive(elemRef), "reading an element kept it alive")

' 3. FOR EACH binds each element without keeping it alive.
DIM member AS Item
DIM i AS INTEGER
DIM lastElemRef AS OBJECT
FOR i = 0 TO 2
    items(i) = NEW Item()
    items(i).label = "e" + STR$(i)
NEXT i
lastElemRef = Zanna.Memory.WeakRef.New(items(2))
seen = ""
FOR EACH member IN items
    seen = seen + member.label
NEXT member
Check(seen = "e0e1e2", "FOR EACH labels: " + seen)
items(2) = NOTHING
Check(Zanna.Memory.WeakRef.IsAlive(lastElemRef), "FOR EACH variable lost its element")
member = NOTHING
Check(NOT Zanna.Memory.WeakRef.IsAlive(lastElemRef), "FOR EACH kept an element alive")

' 4. Parameters are owned by the procedure.
DIM original AS Item
original = NEW Item()
original.label = "original"
Replace(original)
Check(original.label = "original", "object parameter assignment reached the caller")
Check(NOT Zanna.Memory.WeakRef.IsAlive(lastRef), "object assigned to a parameter outlived its SUB")

DIM word AS STRING
word = "base" + STR$(7)
Suffix(word)
Check(word = "base7", "string parameter assignment reached the caller: " + word)
original.Rename(word)
Check(word = "base7" AND original.label = "base7!", "method string parameter: " + original.label)

' 5. FUNCTION results and exits release locals.
DIM round AS INTEGER
DIM greeting$
FOR round = 1 TO 50
    greeting$ = Greeting("x" + STR$(round))
NEXT round
Check(greeting$ = "hi x50", "function-name result: " + greeting$)
Check(FirstLabel(original) = "base7!/<base7!>", "RETURN with locals: " + FirstLabel(original))
DIM blank AS Item
blank = NEW Item()
Check(FirstLabel(blank) = "empty", "EXIT FUNCTION with locals")
EarlyOut(TRUE)
Check(NOT Zanna.Memory.WeakRef.IsAlive(lastRef), "bare RETURN skipped local cleanup")
EarlyOut(FALSE)
Check(NOT Zanna.Memory.WeakRef.IsAlive(lastRef), "SUB end skipped local cleanup")

' 6. USING over an existing object leaves it alive.
DIM kept AS Item
DIM keptRef AS OBJECT
kept = NEW Item()
kept.label = "kept"
keptRef = Zanna.Memory.WeakRef.New(kept)
USING borrowed AS Item = kept
    seen = borrowed.label
END USING
Check(Zanna.Memory.WeakRef.IsAlive(keptRef) AND kept.label = "kept", "USING released a borrowed object")

Check(seen = "kept", "USING variable member read: " + seen)

' 7. A CATCH variable holds the handled error's code.
DIM caughtCode AS INTEGER
caughtCode = -1
TRY
    OPEN "/nonexistent/zanna_missing_file.txt" FOR INPUT AS #1
CATCH problem
    caughtCode = problem
END TRY
Check(caughtCode = 1, "CATCH variable code: " + STR$(caughtCode))

' 8. String builtins that build strings.
Check(HEX$(255) = "FF" AND OCT$(8) = "10", "HEX$/OCT$")
Check(SPACE$(3) = "   " AND STRING$(2, "ab") = "aa" AND STRING$(3, 42) = "***", "SPACE$/STRING$")

' 9. Every kind of STRING storage starts as "".
Check(GlobalIsEmpty(), "module STRING read in a SUB")
Check(StaticIsEmpty() AND NOT StaticIsEmpty(), "STATIC STRING local")
DIM names(2) AS STRING
Check(names(1) = "" AND LEFT$(names(2), 1) = "", "STRING array element")
DIM shelf AS Shelf
shelf = NEW Shelf()
Check(shelf.title = "" AND shelf.labels(0) = "" AND Shelf.motto = "", "STRING fields")

IF fails = 0 THEN
    PRINT "RESULT: ok"
ELSE
    PRINT "RESULT: "; fails; " failure(s)"
END IF
