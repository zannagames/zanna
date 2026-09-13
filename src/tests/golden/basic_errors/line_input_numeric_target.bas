REM LINE INPUT # reads a whole line, so its destination must be a STRING variable.
OPEN "tmp_line_input_numeric_target.txt" FOR OUTPUT AS #1
PRINT #1, "12"
CLOSE #1
OPEN "tmp_line_input_numeric_target.txt" FOR INPUT AS #1
LINE INPUT #1, N
CLOSE #1
END
