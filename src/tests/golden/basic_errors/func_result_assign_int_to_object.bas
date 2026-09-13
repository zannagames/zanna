REM The name of a FUNCTION declared AS a class is an object result slot, so assigning a
REM number to it is B2001.
CLASS Box
    PUBLIC v AS INTEGER
END CLASS
FUNCTION MAKEBOX() AS Box
    MAKEBOX = 5
END FUNCTION
PRINT 1
END
