' ===----------------------------------------------------------------------===
'
' Part of the Zanna project, under the GNU GPL v3.
' See LICENSE for license information.
'
' ===----------------------------------------------------------------------===
'
' File: src/tests/fixtures/runtime/test_basic_runtime_result_ownership.bas
' Purpose: Prove that BASIC honours the declared result ownership of runtime
'          functions (ADR 0314) end to end: an owned result dies with its last
'          BASIC owner whether it came from a qualified call, a method call or
'          a nested argument; a borrowed result never disturbs the object that
'          owns it; and a borrowed string read straight off a temporary Option
'          or a dead Result stays valid.
' Key invariants:
'   - WeakRef.IsAlive turns false exactly when the last owner drops an owned
'     result (Mesh3D.Box, Material3D.PBR, CompiledPattern.Find).
'   - Reading a borrowed accessor (Entity3D.get_Mesh / Entity3D.Mesh) and
'     dropping the value leaves the entity's mesh alive.
'   - Option.UnwrapStr on a temporary and Result.UnwrapStr after the Result
'     dies both keep their text.
' Ownership/Lifetime:
'   - Every object is owned by module variables; WeakRefs only observe.
' Links: docs/adr/0314-declared-runtime-result-ownership.md,
'        src/tests/fixtures/runtime/test_runtime_result_ownership.zia
'
' ===----------------------------------------------------------------------===

DIM fails AS INTEGER
fails = 0

' 1. An owned result from a qualified call dies with its owner.
DIM mesh AS OBJECT
mesh = Zanna.Graphics3D.Mesh3D.Box(1.0, 1.0, 1.0)
DIM meshRef AS OBJECT
meshRef = Zanna.Memory.WeakRef.New(mesh)
IF NOT Zanna.Memory.WeakRef.IsAlive(meshRef) THEN
    PRINT "FAIL: mesh dead while owned"
    fails = fails + 1
END IF
mesh = NOTHING
IF Zanna.Memory.WeakRef.IsAlive(meshRef) THEN
    PRINT "FAIL: Mesh3D.Box result survived its owner"
    fails = fails + 1
END IF

' 2. An owned result passed straight into another call dies at the statement end.
DIM matRef AS OBJECT
matRef = Zanna.Memory.WeakRef.New(Zanna.Graphics3D.Material3D.PBR(0.5, 0.5, 0.5))
IF Zanna.Memory.WeakRef.IsAlive(matRef) THEN
    PRINT "FAIL: nested Material3D.PBR result survived its statement"
    fails = fails + 1
END IF

' 3. An owned result from a method call dies with its owner.
DIM pat AS OBJECT
pat = Zanna.Text.CompiledPattern.New("[0-9]+")
DIM found AS OBJECT
found = pat.Find("abc123def")
DIM foundRef AS OBJECT
foundRef = Zanna.Memory.WeakRef.New(found)
found = NOTHING
IF Zanna.Memory.WeakRef.IsAlive(foundRef) THEN
    PRINT "FAIL: CompiledPattern.Find result survived its owner"
    fails = fails + 1
END IF

' 4. Borrowed accessors do not steal the entity's reference.
DIM e AS OBJECT
e = Zanna.Game3D.Entity3D.New()
DIM held AS OBJECT
held = Zanna.Graphics3D.Mesh3D.Box(2.0, 2.0, 2.0)
DIM heldRef AS OBJECT
heldRef = Zanna.Memory.WeakRef.New(held)
Zanna.Game3D.Entity3D.set_Mesh(e, held)
held = NOTHING
DIM i AS INTEGER
DIM got AS OBJECT
FOR i = 1 TO 3
    got = Zanna.Game3D.Entity3D.get_Mesh(e)
    got = e.Mesh
    got = NOTHING
NEXT i
IF NOT Zanna.Memory.WeakRef.IsAlive(heldRef) THEN
    PRINT "FAIL: reading a borrowed mesh released it"
    fails = fails + 1
END IF
e = NOTHING
IF Zanna.Memory.WeakRef.IsAlive(heldRef) THEN
    PRINT "FAIL: mesh survived its entity"
    fails = fails + 1
END IF

' 5. A borrowed string read off a temporary Option stays valid.
DIM digits AS STRING
digits = pat.Find("abc123def").UnwrapStr()
DIM framed AS STRING
framed = "[" + pat.Find("xyz42").UnwrapStr() + "]"
IF digits <> "123" THEN
    PRINT "FAIL: UnwrapStr of a temporary Option: "; digits
    fails = fails + 1
END IF
IF framed <> "[42]" THEN
    PRINT "FAIL: UnwrapStr of a temporary Option in a concatenation: "; framed
    fails = fails + 1
END IF
PRINT "chained: "; pat.Find("abc7").UnwrapStr()

' 6. A borrowed string outlives the Result it was read from.
DIM r AS OBJECT
r = Zanna.Result.OkStr("hello world")
DIM text AS STRING
text = Zanna.Result.UnwrapStr(r)
r = NOTHING
DIM churn AS OBJECT
churn = Zanna.Result.OkStr("churn churn churn")
IF text <> "hello world" THEN
    PRINT "FAIL: UnwrapStr after the Result died: "; text
    fails = fails + 1
END IF

IF fails = 0 THEN
    PRINT "RESULT: ok"
ELSE
    PRINT "RESULT: "; fails; " failure(s)"
END IF
END
