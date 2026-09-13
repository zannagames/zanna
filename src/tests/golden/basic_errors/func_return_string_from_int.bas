REM A FUNCTION without a suffix or AS clause returns INTEGER, so RETURN of a string is B4010
REM instead of reaching the IL verifier.
FUNCTION EXCL(S$)
    RETURN S$ + "!"
END FUNCTION
PRINT EXCL("hi")
END
