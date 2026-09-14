' Reading or writing a channel that was never opened raises an error the
' handler receives; RESUME NEXT continues after the failed statement.
DIM text AS STRING
ON ERROR GOTO Handler
PRINT #1, 42
PRINT "after print"
LINE INPUT #1, text
PRINT "after line input"
END
Handler:
  PRINT "caught"; ERR()
  RESUME NEXT
