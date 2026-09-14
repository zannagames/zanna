' An error raised while the handler runs is not handled again: it ends the
' program with the original error.
DIM zero AS INTEGER
ON ERROR GOTO Handler
PRINT "start"
PRINT 1 \ zero
PRINT "not reached"
END
Handler:
  PRINT "in handler"
  PRINT 2 \ zero
  RESUME NEXT
