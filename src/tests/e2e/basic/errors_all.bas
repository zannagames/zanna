' One handler receives errors of several kinds; RESUME NEXT continues after each.
DIM x AS INTEGER
DIM y AS INTEGER
DIM a(1) AS INTEGER
ON ERROR GOTO Report
x = 1
PRINT x \ y
PRINT "cont1"
OPEN "missing.txt" FOR INPUT AS #1
PRINT "cont2"
PRINT a(3)
PRINT "cont3"
END
Report:
  PRINT "error"; ERR()
  RESUME NEXT
