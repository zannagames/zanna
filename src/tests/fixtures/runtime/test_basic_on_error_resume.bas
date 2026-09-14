' ===----------------------------------------------------------------------===
'
' Part of the Zanna project, under the GNU GPL v3.
' See LICENSE for license information.
'
' ===----------------------------------------------------------------------===
'
' File: src/tests/fixtures/runtime/test_basic_on_error_resume.bas
' Purpose: Prove the BASIC ON ERROR GOTO / RESUME contract end to end: RESUME
'          retries the failed statement, RESUME NEXT continues after it (also
'          inside FOR, FOR EACH, SELECT CASE, and GOSUB bodies), RESUME <label>
'          jumps, ERR reports the handled error, and handlers stay installed
'          after RESUME.
' Key invariants:
'   - An error in a called SUB without a handler reaches the caller's handler,
'     and RESUME NEXT continues after the call.
'   - A SUB, FUNCTION, or method with its own handler resumes inside itself.
'   - A TRY inside a procedure with ON ERROR keeps its own handler; an error in
'     its CATCH body reaches the ON ERROR handler, and RESUME NEXT continues
'     after the TRY statement.
'   - ON ERROR GOTO selects which handler label runs; RESUME without a running
'     handler is itself an error the selected handler receives.
'   - Numeric DIM variables start at zero on every backend.
' Ownership/Lifetime:
'   - Plain module variables; no objects outlive the program.
' Links: src/frontends/basic/lower/Lower_TryCatch.cpp, docs/specs/errors.md,
'        docs/languages/basic-reference.md
'
' ===----------------------------------------------------------------------===

DIM fails AS INTEGER
DIM trail AS STRING
DIM zero AS INTEGER
DIM counter AS INTEGER

SUB Check(ok AS BOOLEAN, what AS STRING)
    IF NOT ok THEN
        PRINT "FAIL: "; what
        fails = fails + 1
    END IF
END SUB

SUB Note(mark AS STRING)
    trail = trail + mark + ";"
END SUB

' Fails without a handler of its own; the caller handles the error.
SUB Divide(n AS INTEGER)
    Note("divide")
    Note("q" + STR$(n \ zero))
    Note("never")
END SUB

' Handles its own error and resumes after the failed statement.
FUNCTION SafeQuotient(a AS INTEGER, b AS INTEGER) AS INTEGER
    ON ERROR GOTO Failed
    SafeQuotient = a \ b
    EXIT FUNCTION
Failed:
    SafeQuotient = -1
    RESUME NEXT
END FUNCTION

' RESUME <label> retries a loop until it succeeds.
SUB Retry()
    DIM attempts AS INTEGER
    ON ERROR GOTO Again
Start:
    attempts = attempts + 1
    IF attempts < 3 THEN Note("fail" + STR$(attempts \ zero))
    Note("retried" + STR$(attempts))
    EXIT SUB
Again:
    Note("again")
    RESUME Start
END SUB

CLASS Worker
    PUBLIC total AS INTEGER

    SUB Work(d AS INTEGER)
        ON ERROR GOTO WorkFailed
        total = total + 10 \ d
        Note("worked" + STR$(total))
        EXIT SUB
WorkFailed:
        Note("method")
        RESUME NEXT
    END SUB
END CLASS

' Zero-initialised locals.
DIM untouched AS INTEGER
DIM untouchedF AS DOUBLE
Check(untouched = 0, "DIM INTEGER starts at zero")
Check(untouchedF = 0.0, "DIM DOUBLE starts at zero")

ON ERROR GOTO Handler

' 1. RESUME retries the failed statement once the handler fixes the cause.
DIM divisor AS INTEGER
DIM result AS INTEGER
result = 12 \ divisor
Check(result = 6, "RESUME retried with the fixed divisor: " + STR$(result))
Check(ERR() = 0, "ERR is reset after RESUME")

' 2. RESUME NEXT inside FOR and IF continues with the next statement in the loop.
DIM i AS INTEGER
trail = ""
FOR i = 1 TO 3
    IF i = 2 THEN Note("bad" + STR$(i \ zero))
    Note("i" + STR$(i))
NEXT i
Check(trail = "i1;handled;i2;i3;", "FOR/IF trail: " + trail)

' 3. SELECT CASE with a string selector and FOR EACH.
DIM words(2) AS STRING
words(0) = "a"
words(1) = "boom"
words(2) = "c"
trail = ""
FOR i = 0 TO 2
    SELECT CASE UCASE$(words(i))
        CASE "BOOM"
            Note("case" + STR$(1 \ zero))
            Note("after-case")
        CASE ELSE
            Note(words(i))
    END SELECT
NEXT i
DIM word AS STRING
FOR EACH word IN words
    IF word = "c" THEN Note("each" + STR$(2 \ zero))
    Note("e" + word)
NEXT word
Check(trail = "a;handled;after-case;c;ea;eboom;handled;ec;", "SELECT/FOR EACH trail: " + trail)

' 4. An error in a called SUB reaches this handler; RESUME NEXT continues after the call.
trail = ""
Divide(7)
Note("after-call")
Check(trail = "divide;handled;after-call;", "caller trail: " + trail)

' 5. Procedures and methods with their own handlers.
Check(SafeQuotient(10, 2) = 5, "SafeQuotient(10, 2)")
Check(SafeQuotient(10, 0) = -1, "SafeQuotient(10, 0)")
trail = ""
Retry()
Check(trail = "again;again;retried3;", "RESUME label trail: " + trail)
DIM w AS Worker
w = NEW Worker()
trail = ""
w.Work(2)
w.Work(0)
Check(trail = "worked5;method;worked5;", "method trail: " + trail)

' 6. TRY keeps its own handler; an error in CATCH reaches ON ERROR.
trail = ""
TRY
    Note("try")
    Note("t" + STR$(1 \ zero))
CATCH
    Note("catch")
    Note("c" + STR$(1 \ zero))
    Note("not-after-catch-error")
END TRY
Note("after-try")
Check(trail = "try;catch;handled;after-try;", "TRY trail: " + trail)

' 7. GOSUB bodies resume inside the subroutine.
trail = ""
GOSUB Sub1
Note("after-gosub")
Check(trail = "gosub;handled;resumed-in-gosub;after-gosub;", "GOSUB trail: " + trail)

' 8. ON ERROR GOTO selects the handler; a stray RESUME is an error for it.
ON ERROR GOTO Other
trail = ""
counter = 5 \ zero
Note("after-other")
RESUME NEXT
Note("after-stray-resume")
Check(trail = "other0;after-other;other0;after-stray-resume;", "handler switch trail: " + trail)

IF fails = 0 THEN
    PRINT "RESULT: ok"
ELSE
    PRINT "RESULT: "; fails; " failure(s)"
END IF
END

Sub1:
Note("gosub")
Note("g" + STR$(3 \ zero))
Note("resumed-in-gosub")
RETURN

Handler:
Note("handled")
IF divisor = 0 AND result = 0 AND counter = 0 THEN
    divisor = 2
    counter = 1
    Check(ERR() = 0, "ERR for division by zero is its code 0")
    RESUME
END IF
RESUME NEXT

Other:
Note("other" + STR$(ERR()))
RESUME NEXT
