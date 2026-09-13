REM An OBJECT variable takes the class of the runtime value assigned to it, and a variable
REM declared AS a class keeps that class when an untyped OBJECT is assigned to it.
FUNCTION MakeResult() AS OBJECT
    RETURN Zanna.Result.OkStr("made")
END FUNCTION
DIM pat AS OBJECT
pat = Zanna.Text.CompiledPattern.New("[0-9]+")
PRINT pat.Find("abc7").UnwrapStr()
DIM r AS OBJECT
r = MakeResult()
DIM typed AS Zanna.Result
typed = r
PRINT typed.UnwrapStr()
END
