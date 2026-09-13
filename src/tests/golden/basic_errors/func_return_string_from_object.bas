REM RETURN from a FUNCTION declared AS a class must return an object, not a string.
CLASS Box
    PUBLIC v AS INTEGER
END CLASS
FUNCTION MAKEBOX() AS Box
    RETURN "text"
END FUNCTION
PRINT 1
END
