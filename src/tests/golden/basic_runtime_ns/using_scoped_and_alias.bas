REM USING works at file scope and inside a NAMESPACE, as an import and as an alias:
REM unqualified calls, alias-qualified calls, NEW, DIM, parameters, and results.
USING U = App.Utils
USING T = Zanna.Terminal
NAMESPACE App.Utils
  SUB Hello()
    PRINT "hello"
  END SUB
  FUNCTION Twice(n AS INTEGER) AS INTEGER
    Twice = n * 2
  END FUNCTION
  SUB CallsSibling()
    Hello()
    PRINT "sibling"; Twice(4)
  END SUB
END NAMESPACE
NAMESPACE Other
  USING App.Utils
  USING K = App.Utils
  USING Zanna.Text
  SUB Go()
    Hello()
    K.Hello()
    PRINT "twice"; Twice(5); K.Twice(6)
    DIM sb AS StringBuilder
    sb = NEW StringBuilder()
    sb.Append("built")
    PRINT sb.ToString()
  END SUB
END NAMESPACE
U.Hello()
PRINT "U.Twice"; U.Twice(3)
DIM x AS INTEGER
x = U.Twice(10) + 1
PRINT x
T.PrintI64(5)
PRINT ""
App.Utils.CallsSibling()
Other.Go()
NAMESPACE Builders
  USING Zanna.Text
  SUB Fill(sb AS StringBuilder)
    sb.Append("filled")
  END SUB
  FUNCTION Make() AS StringBuilder
    DIM b AS StringBuilder
    b = NEW StringBuilder()
    Fill(b)
    RETURN b
  END FUNCTION
END NAMESPACE
DIM made AS Zanna.Text.StringBuilder
made = Builders.Make()
PRINT made.ToString()
