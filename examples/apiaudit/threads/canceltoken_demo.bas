' =============================================================================
' API Audit: Zanna.Threads.CancelToken (BASIC)
' =============================================================================
' Tests: New, IsCancelled, Cancel, Reset, Check, Linked
' =============================================================================

PRINT "=== API Audit: Zanna.Threads.CancelToken ==="

' --- New ---
PRINT "--- New ---"
DIM token AS OBJECT = Zanna.Threads.CancelToken.New()
PRINT "Created token"

' --- IsCancelled (initial) ---
PRINT "--- IsCancelled (initial) ---"
PRINT "IsCancelled: "; token.IsCancelled

' --- Cancel ---
PRINT "--- Cancel ---"
token.Cancel()
PRINT "IsCancelled after Cancel: "; token.IsCancelled

' --- Reset ---
PRINT "--- Reset ---"
token.Reset()
PRINT "IsCancelled after Reset: "; token.IsCancelled

' --- Check (not cancelled) ---
PRINT "--- Check (not cancelled) ---"
PRINT "Check: "; token.IsCancelled

' --- Cancel and Check ---
PRINT "--- Cancel and Check ---"
token.Cancel()
PRINT "Check after Cancel: "; token.IsCancelled

' --- Reset for linked test ---
token.Reset()

' --- Linked ---
' Note: RT_METHOD "Linked" signature mismatch with RT_FUNC - use function-style call
PRINT "--- Linked ---"
DIM child AS OBJECT = Zanna.Threads.CancelToken.Linked(token)
PRINT "Child IsCancelled: "; child.IsCancelled
PRINT "Child Check: "; child.IsCancelled

' --- Linked: cancel parent ---
PRINT "--- Linked: cancel parent ---"
token.Cancel()
PRINT "Parent IsCancelled: "; token.IsCancelled
PRINT "Child Check (parent cancelled): "; child.IsCancelled

' --- Linked: independent cancel ---
PRINT "--- Linked: independent cancel ---"
token.Reset()
DIM child2 AS OBJECT = Zanna.Threads.CancelToken.Linked(token)
child2.Cancel()
PRINT "Child2 IsCancelled: "; child2.IsCancelled
PRINT "Parent IsCancelled: "; token.IsCancelled

PRINT "=== CancelToken Audit Complete ==="
END
