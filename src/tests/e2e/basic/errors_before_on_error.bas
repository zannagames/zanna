' An error raised before ON ERROR GOTO runs is unhandled even though the
' procedure has a handler later on.
DIM zero AS INTEGER
PRINT "start"
PRINT 1 \ zero
ON ERROR GOTO Handler
PRINT "not reached"
END
Handler:
  PRINT "handler must not run"
  RESUME NEXT
