OPEN "tmp_fileio_eof.txt" FOR OUTPUT AS #1
PRINT #1, "x"
CLOSE #1
OPEN "tmp_fileio_eof.txt" FOR INPUT AS #1
PRINT EOF(#1)
LINE INPUT #1, A$
PRINT EOF(#1)
CLOSE #1
