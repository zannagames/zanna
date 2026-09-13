REM Inside a class member, a field of the receiver hides a module-level variable with the
REM same name for reads, writes and RETURN; code outside the class still sees the module
REM variable.
DIM count AS INTEGER
count = 100
CLASS Counter
    PUBLIC count AS INTEGER
    PUBLIC SUB Bump()
        count = count + 1
    END SUB
    PUBLIC FUNCTION Current() AS INTEGER
        RETURN count
    END FUNCTION
END CLASS
DIM c AS Counter
c = NEW Counter()
c.Bump()
c.Bump()
PRINT c.Current(); " "; c.count; " "; count
END
