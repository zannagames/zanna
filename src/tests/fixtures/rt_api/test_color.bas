' test_color.bas — Zanna.Graphics.Color (no window needed)
DIM c AS INTEGER
LET c = Zanna.Graphics.Color.RGB(255, 128, 0)
PRINT Zanna.Graphics.Color.GetRed(c)
PRINT Zanna.Graphics.Color.GetGreen(c)
PRINT Zanna.Graphics.Color.GetBlue(c)
PRINT Zanna.Graphics.Color.GetAlpha(c)
PRINT Zanna.Graphics.Color.ToHex(c)

DIM c2 AS INTEGER
LET c2 = Zanna.Graphics.Color.FromHex("#ff8000")
PRINT Zanna.Graphics.Color.GetRed(c2)

DIM c3 AS INTEGER
LET c3 = Zanna.Graphics.Color.RGBA(100, 200, 50, 128)
PRINT Zanna.Graphics.Color.GetAlpha(c3)

DIM c4 AS INTEGER
LET c4 = Zanna.Graphics.Color.Grayscale(c)
PRINT Zanna.Graphics.Color.GetRed(c4)

DIM c5 AS INTEGER
LET c5 = Zanna.Graphics.Color.Invert(Zanna.Graphics.Color.RGB(0, 0, 0))
PRINT Zanna.Graphics.Color.GetRed(c5)

DIM c6 AS INTEGER
LET c6 = Zanna.Graphics.Color.Complement(c)
PRINT Zanna.Graphics.Color.GetRed(c6)

DIM c7 AS INTEGER
LET c7 = Zanna.Graphics.Color.Lerp(Zanna.Graphics.Color.RGB(0,0,0), Zanna.Graphics.Color.RGB(255,255,255), 50)
PRINT Zanna.Graphics.Color.GetRed(c7)

DIM c8 AS INTEGER
LET c8 = Zanna.Graphics.Color.Brighten(c, 50)
PRINT Zanna.Graphics.Color.GetRed(c8)

DIM c9 AS INTEGER
LET c9 = Zanna.Graphics.Color.Darken(c, 50)
PRINT Zanna.Graphics.Color.GetRed(c9)

PRINT "done"
END
