REM The name of an INTEGER FUNCTION is its result slot: it keeps the declared type instead of
REM adopting the assigned value's, so assigning a string to it is B2001.
FUNCTION EXCL(S$)
    EXCL = S$ + "!"
END FUNCTION
PRINT EXCL("hi")
END
