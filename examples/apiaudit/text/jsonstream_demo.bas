' Zanna.Data.JsonStream API Audit - Streaming JSON Parser
' Tests all JsonStream functions

PRINT "=== Zanna.Data.JsonStream API Audit ==="

' --- New / NextResult / TokenType ---
PRINT "--- New / NextResult / TokenType ---"
DIM stream AS OBJECT
stream = Zanna.Data.JsonStream.New("{""name"":""Alice"",""age"":30,""active"":true}")

DIM i AS INTEGER
DIM nextToken AS OBJECT
i = 0
DO WHILE i < 20
    IF NOT Zanna.Data.JsonStream.HasNext(stream) THEN
        i = 100
    ELSE
        nextToken = Zanna.Data.JsonStream.NextResult(stream)
        IF nextToken.IsErr THEN
            PRINT "Token error: "; nextToken.UnwrapErrStr()
            i = 100
        ELSE
            PRINT "Token type: "; nextToken.UnwrapI64()
        END IF
        i = i + 1
    END IF
LOOP

' --- StringValue ---
PRINT "--- StringValue ---"
DIM s2 AS OBJECT
s2 = Zanna.Data.JsonStream.New("{""greeting"":""hello""}")
Zanna.Data.JsonStream.Next(s2)
Zanna.Data.JsonStream.Next(s2)
PRINT "Key: "; Zanna.Data.JsonStream.StringValue(s2)
Zanna.Data.JsonStream.Next(s2)
PRINT "Value: "; Zanna.Data.JsonStream.StringValue(s2)

' --- NumberValue ---
PRINT "--- NumberValue ---"
DIM s3 AS OBJECT
s3 = Zanna.Data.JsonStream.New("{""value"":42.5}")
Zanna.Data.JsonStream.Next(s3)
Zanna.Data.JsonStream.Next(s3)
Zanna.Data.JsonStream.Next(s3)
PRINT "Number: "; Zanna.Data.JsonStream.NumberValue(s3)

' --- BoolValue ---
PRINT "--- BoolValue ---"
DIM s4 AS OBJECT
s4 = Zanna.Data.JsonStream.New("{""flag"":true}")
Zanna.Data.JsonStream.Next(s4)
Zanna.Data.JsonStream.Next(s4)
Zanna.Data.JsonStream.Next(s4)
PRINT "Bool: "; Zanna.Data.JsonStream.BoolValue(s4)

' --- Depth ---
PRINT "--- Depth ---"
DIM s5 AS OBJECT
s5 = Zanna.Data.JsonStream.New("{""nested"":{""deep"":1}}")
Zanna.Data.JsonStream.Next(s5)
PRINT "Depth at {: "; Zanna.Data.JsonStream.Depth(s5)
Zanna.Data.JsonStream.Next(s5)
Zanna.Data.JsonStream.Next(s5)
PRINT "Depth at nested {: "; Zanna.Data.JsonStream.Depth(s5)

' --- Skip ---
PRINT "--- Skip ---"
DIM s6 AS OBJECT
s6 = Zanna.Data.JsonStream.New("{""skip"":[1,2,3],""keep"":""yes""}")
Zanna.Data.JsonStream.Next(s6)
Zanna.Data.JsonStream.Next(s6)
Zanna.Data.JsonStream.Skip(s6)
Zanna.Data.JsonStream.Next(s6)
PRINT "After skip key: "; Zanna.Data.JsonStream.StringValue(s6)
Zanna.Data.JsonStream.Next(s6)
PRINT "After skip val: "; Zanna.Data.JsonStream.StringValue(s6)

' --- HasNext ---
PRINT "--- HasNext ---"
DIM s7 AS OBJECT
s7 = Zanna.Data.JsonStream.New("42")
PRINT "HasNext before: "; Zanna.Data.JsonStream.HasNext(s7)
Zanna.Data.JsonStream.Next(s7)
PRINT "HasNext after: "; Zanna.Data.JsonStream.HasNext(s7)

' --- NextResult ---
PRINT "--- NextResult ---"
DIM s8 AS OBJECT
s8 = Zanna.Data.JsonStream.New("not json")
DIM bad AS OBJECT
bad = Zanna.Data.JsonStream.NextResult(s8)
PRINT "Bad NextResult IsErr: "; bad.IsErr
PRINT "Bad NextResult Err: "; bad.UnwrapErrStr()

PRINT "=== JsonStream Demo Complete ==="
END
