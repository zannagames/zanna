OPEN "tmp_fileio_input_num.txt" FOR OUTPUT AS #1
PRINT #1, "7"
PRINT #1, "3.14"
CLOSE #1
OPEN "tmp_fileio_input_num.txt" FOR INPUT AS #1
INPUT #1, A%
INPUT #1, B#
PRINT A%
PRINT B#
CLOSE #1
