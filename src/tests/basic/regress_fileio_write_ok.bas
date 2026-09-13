OPEN "tmp_regress_fileio_write.txt" FOR OUTPUT AS #1
WRITE #1, "A", 42, "B"
CLOSE #1
PRINT "ok"
