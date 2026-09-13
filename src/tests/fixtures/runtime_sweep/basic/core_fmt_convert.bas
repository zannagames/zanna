' EXPECT_OUT: RESULT: ok
' COVER: Zanna.Core.Convert.ToDouble
' COVER: Zanna.Core.Convert.ToInt64
' COVER: Zanna.Core.Convert.ToStringDouble
' COVER: Zanna.Core.Convert.ToStringInt
' COVER: Zanna.Text.Fmt.Bin
' COVER: Zanna.Text.Fmt.Bool
' COVER: Zanna.Text.Fmt.Bool
' COVER: Zanna.Text.Fmt.Hex
' COVER: Zanna.Text.Fmt.HexPad
' COVER: Zanna.Text.Fmt.Int
' COVER: Zanna.Text.Fmt.IntPad
' COVER: Zanna.Text.Fmt.IntRadix
' COVER: Zanna.Text.Fmt.Num
' COVER: Zanna.Text.Fmt.NumFixed
' COVER: Zanna.Text.Fmt.Percent
' COVER: Zanna.Text.Fmt.Scientific
' COVER: Zanna.Text.Fmt.Oct
' COVER: Zanna.Text.Fmt.SizeBytes

SUB AssertApprox(actual AS DOUBLE, expected AS DOUBLE, eps AS DOUBLE, msg AS STRING)
    IF Zanna.Math.Abs(actual - expected) > eps THEN
        Zanna.Core.Diagnostics.Assert(FALSE, msg)
    END IF
END SUB

DIM d AS DOUBLE
d = Zanna.Core.Convert.ToDouble("3.5")
AssertApprox(d, 3.5, 0.000001, "conv.double")
Zanna.Core.Diagnostics.AssertEq(Zanna.Core.Convert.ToInt64("42"), 42, "conv.int")
Zanna.Core.Diagnostics.AssertEqStr(Zanna.Core.Convert.ToStringInt(42), "42", "conv.toStrInt")
Zanna.Core.Diagnostics.AssertEqStr(Zanna.Core.Convert.ToStringDouble(2.5), "2.5", "conv.toStrDouble")

Zanna.Core.Diagnostics.AssertEqStr(Zanna.Text.Fmt.Int(12345), "12345", "fmt.int")
Zanna.Core.Diagnostics.AssertEqStr(Zanna.Text.Fmt.IntRadix(255, 16), "ff", "fmt.radix")
Zanna.Core.Diagnostics.AssertEqStr(Zanna.Text.Fmt.IntPad(42, 5, "0"), "00042", "fmt.intpad")
Zanna.Core.Diagnostics.AssertEqStr(Zanna.Text.Fmt.Num(3.14159), "3.14159", "fmt.num")
Zanna.Core.Diagnostics.AssertEqStr(Zanna.Text.Fmt.NumFixed(3.14159, 2), "3.14", "fmt.numfixed")
Zanna.Core.Diagnostics.AssertEqStr(Zanna.Text.Fmt.Scientific(1234.5, 2), "1.23e+03", "fmt.numsci")
Zanna.Core.Diagnostics.AssertEqStr(Zanna.Text.Fmt.Percent(0.756, 1), "75.6%", "fmt.numpct")
Zanna.Core.Diagnostics.AssertEqStr(Zanna.Text.Fmt.Bool(TRUE), "true", "fmt.bool")
Zanna.Core.Diagnostics.AssertEqStr(Zanna.Text.Fmt.YesNo(FALSE), "no", "fmt.boolyn")
Zanna.Core.Diagnostics.AssertEqStr(Zanna.Text.Fmt.SizeBytes(1024), "1.0 KB", "fmt.size")
Zanna.Core.Diagnostics.AssertEqStr(Zanna.Text.Fmt.Hex(255), "ff", "fmt.hex")
Zanna.Core.Diagnostics.AssertEqStr(Zanna.Text.Fmt.HexPad(255, 4), "00ff", "fmt.hexpad")
Zanna.Core.Diagnostics.AssertEqStr(Zanna.Text.Fmt.Bin(10), "1010", "fmt.bin")
Zanna.Core.Diagnostics.AssertEqStr(Zanna.Text.Fmt.Oct(64), "100", "fmt.oct")

PRINT "RESULT: ok"
END
