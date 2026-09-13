OPEN "tmp_fileio_lof.txt" FOR OUTPUT AS #1
PRINT #1, "AB"
CLOSE #1
OPEN "tmp_fileio_lof.txt" FOR INPUT AS #1
IF LOF(#1) >= 2 THEN PRINT "OK" ELSE PRINT "BAD"
CLOSE #1
