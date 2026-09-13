OPEN "tmp_fileio_write.txt" FOR OUTPUT AS #1
WRITE #1, "A", 42, "B"
CLOSE #1
OPEN "tmp_fileio_write.txt" FOR INPUT AS #1
LINE INPUT #1, S$
PRINT S$
CLOSE #1
