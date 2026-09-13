OPEN "tmp_fileio_eof_lof_loc.txt" FOR OUTPUT AS #1
WRITE #1, "X"
CLOSE #1

OPEN "tmp_fileio_eof_lof_loc.txt" FOR INPUT AS #1
PRINT "EOF start:"; EOF(#1)  ' expect 0
LINE INPUT #1, A$
PRINT "AFTER READ:"; A$
PRINT "EOF end:"; EOF(#1)    ' expect -1
PRINT "LOF positive:"; LOF(#1) > 0
PRINT "LOC not negative:"; LOC(#1) >= 0
CLOSE #1
