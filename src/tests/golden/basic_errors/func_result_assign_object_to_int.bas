REM An INTEGER FUNCTION's result slot cannot hold an object.
CLASS Box
    PUBLIC v AS INTEGER
END CLASS
FUNCTION COUNT()
    DIM b AS Box
    b = NEW Box()
    COUNT = b
END FUNCTION
PRINT COUNT()
END
