OPEN "tmp_fileio_input_str.txt" FOR OUTPUT AS #1
PRINT #1, "Alice,42"
CLOSE #1
OPEN "tmp_fileio_input_str.txt" FOR INPUT AS #1
INPUT #1, N$
PRINT N$
CLOSE #1
