' A missing file raises FileNotFound; the handler reports ERR and RESUME NEXT
' continues after the OPEN.
ON ERROR GOTO Handler
OPEN "does_not_exist.txt" FOR INPUT AS #1
PRINT "after open"
END
Handler:
  PRINT "caught"; ERR()
  RESUME NEXT
