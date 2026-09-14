REM STRING$ traps on a character code outside 0-255.
DIM code AS INTEGER
code = 256
PRINT STRING$(3, code)
