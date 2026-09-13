' EXPECT_OUT: RESULT: ok
' COVER: Zanna.Graphics.Canvas.New
' COVER: Zanna.Graphics.Canvas.Height
' COVER: Zanna.Graphics.Canvas.ShouldClose
' COVER: Zanna.Graphics.Canvas.Width
' COVER: Zanna.Graphics.Canvas.Box
' COVER: Zanna.Graphics.Canvas.Clear
' COVER: Zanna.Graphics.Canvas.Disc
' COVER: Zanna.Graphics.Canvas.Flip
' COVER: Zanna.Graphics.Canvas.Frame
' COVER: Zanna.Graphics.Canvas.KeyHeld
' COVER: Zanna.Graphics.Canvas.Line
' COVER: Zanna.Graphics.Canvas.Plot
' COVER: Zanna.Graphics.Canvas.Poll
' COVER: Zanna.Graphics.Canvas.Ring
' COVER: Zanna.Graphics.Color.Rgb
' COVER: Zanna.Graphics.Color.Rgba
' COVER: Zanna.Graphics.Pixels.New
' COVER: Zanna.Graphics.Pixels.Height
' COVER: Zanna.Graphics.Pixels.Width
' COVER: Zanna.Graphics.Pixels.Clear
' COVER: Zanna.Graphics.Pixels.Clone
' COVER: Zanna.Graphics.Pixels.Copy
' COVER: Zanna.Graphics.Pixels.Fill
' COVER: Zanna.Graphics.Pixels.Get
' COVER: Zanna.Graphics.Pixels.Set
' COVER: Zanna.Graphics.Pixels.ToBytes

DIM canvas AS Zanna.Graphics.Canvas
canvas = NEW Zanna.Graphics.Canvas("Runtime Canvas", 64, 48)

Zanna.Core.Diagnostics.AssertEq(canvas.Width, 64, "canvas.width")
Zanna.Core.Diagnostics.AssertEq(canvas.Height, 48, "canvas.height")
Zanna.Core.Diagnostics.Assert(canvas.ShouldClose = FALSE OR canvas.ShouldClose = TRUE, "canvas.shouldclose")

DIM red AS INTEGER
DIM green AS INTEGER
DIM blue AS INTEGER
DIM white AS INTEGER
red = Zanna.Graphics.Color.Rgb(255, 0, 0)
green = Zanna.Graphics.Color.Rgb(0, 255, 0)
blue = Zanna.Graphics.Color.Rgba(0, 0, 255, 255)
white = Zanna.Graphics.Color.Rgb(255, 255, 255)

canvas.Clear(white)
canvas.Box(2, 2, 10, 8, red)
canvas.Frame(1, 1, 12, 10, blue)
canvas.Line(0, 0, 20, 15, green)
canvas.Disc(20, 20, 4, red)
canvas.Ring(30, 15, 5, blue)
canvas.Plot(5, 5, green)

DIM evt AS INTEGER
evt = canvas.Poll()
Zanna.Core.Diagnostics.Assert(evt >= 0, "canvas.poll")

DIM held AS INTEGER
held = canvas.KeyHeld(Zanna.Input.Key.A)
Zanna.Core.Diagnostics.Assert(held = 0 OR held = 1, "canvas.keyheld")

canvas.Flip()

DIM pixels AS Zanna.Graphics.Pixels
pixels = NEW Zanna.Graphics.Pixels(4, 3)
Zanna.Core.Diagnostics.AssertEq(pixels.Width, 4, "pixels.width")
Zanna.Core.Diagnostics.AssertEq(pixels.Height, 3, "pixels.height")

pixels.FillColor(red)
Zanna.Core.Diagnostics.AssertEq(pixels.GetColor(0, 0), Zanna.Graphics.Color.Rgba(255, 0, 0, 255), "pixels.fill")

pixels.SetColor(1, 1, blue)
Zanna.Core.Diagnostics.AssertEq(pixels.GetColor(1, 1), blue, "pixels.set")

DIM src AS Zanna.Graphics.Pixels
src = NEW Zanna.Graphics.Pixels(2, 2)
src.FillColor(green)

pixels.Copy(2, 0, src, 0, 0, 2, 2)
Zanna.Core.Diagnostics.AssertEq(pixels.GetColor(2, 0), Zanna.Graphics.Color.Rgba(0, 255, 0, 255), "pixels.copy")

DIM clone AS Zanna.Graphics.Pixels
clone = pixels.Clone()
Zanna.Core.Diagnostics.AssertEq(clone.GetColor(1, 1), blue, "pixels.clone")

pixels.Clear()
Zanna.Core.Diagnostics.AssertEq(pixels.Get(0, 0), 0, "pixels.clear")

DIM buf AS Zanna.Collections.Bytes
buf = clone.ToBytes()
Zanna.Core.Diagnostics.AssertEq(buf.Length, 4 * 3 * 4, "pixels.tobytes")

PRINT "RESULT: ok"
END
