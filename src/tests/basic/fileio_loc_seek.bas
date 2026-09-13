OPEN "tmp_fileio_loc_seek.txt" FOR OUTPUT AS #1
PRINT #1, "HELLO"
CLOSE #1
OPEN "tmp_fileio_loc_seek.txt" FOR INPUT AS #1
PRINT LOC(#1)
SEEK #1, 2
LINE INPUT #1, S$
PRINT S$
CLOSE #1
