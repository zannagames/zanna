' EXPECT_OUT: RESULT: ok
' COVER: Zanna.Graphics.Color.FromHsl
' COVER: Zanna.Graphics.Color.Lerp
' COVER: Zanna.Graphics.Color.GetRed
' COVER: Zanna.Graphics.Color.GetGreen
' COVER: Zanna.Graphics.Color.GetBlue
' COVER: Zanna.Graphics.Color.GetAlpha
' COVER: Zanna.Graphics.Color.Brighten
' COVER: Zanna.Graphics.Color.Darken
' COVER: Zanna.Graphics.Pixels.Invert
' COVER: Zanna.Graphics.Pixels.Grayscale
' COVER: Zanna.Graphics.Pixels.Tint
' COVER: Zanna.Graphics.Pixels.Blur
' COVER: Zanna.Graphics.Pixels.Resize
' COVER: Zanna.Graphics.Canvas.GradientH
' COVER: Zanna.Graphics.Canvas.GradientV
' COVER: Zanna.Graphics.Sprite.New
' COVER: Zanna.Graphics.Sprite.X
' COVER: Zanna.Graphics.Sprite.Y
' COVER: Zanna.Graphics.Sprite.Width
' COVER: Zanna.Graphics.Sprite.Height
' COVER: Zanna.Graphics.Sprite.Visible
' COVER: Zanna.Graphics.Sprite.ScaleX
' COVER: Zanna.Graphics.Sprite.ScaleY
' COVER: Zanna.Graphics.Sprite.Frame
' COVER: Zanna.Graphics.Sprite.FrameCount
' COVER: Zanna.Graphics.Sprite.Draw
' COVER: Zanna.Graphics.Sprite.Move
' COVER: Zanna.Graphics.Sprite.Contains
' COVER: Zanna.Graphics2D.Tilemap.New
' COVER: Zanna.Graphics2D.Tilemap.Width
' COVER: Zanna.Graphics2D.Tilemap.Height
' COVER: Zanna.Graphics2D.Tilemap.TileWidth
' COVER: Zanna.Graphics2D.Tilemap.TileHeight
' COVER: Zanna.Graphics2D.Tilemap.SetTile
' COVER: Zanna.Graphics2D.Tilemap.GetTile
' COVER: Zanna.Graphics2D.Tilemap.Fill
' COVER: Zanna.Graphics2D.Tilemap.Clear
' COVER: Zanna.Graphics.Camera.New
' COVER: Zanna.Graphics.Camera.X
' COVER: Zanna.Graphics.Camera.Y
' COVER: Zanna.Graphics.Camera.Width
' COVER: Zanna.Graphics.Camera.Height
' COVER: Zanna.Graphics.Camera.Zoom
' COVER: Zanna.Graphics.Camera.Move
' COVER: Zanna.Graphics.Camera.ToScreenX
' COVER: Zanna.Graphics.Camera.ToScreenY
' COVER: Zanna.Graphics.Camera.ToWorldX
' COVER: Zanna.Graphics.Camera.ToWorldY
' COVER: Zanna.GUI.Align.Start
' COVER: Zanna.GUI.Justify.SpaceEvenly
' COVER: Zanna.GUI.FlexDirection.Column
' COVER: Zanna.GUI.FlexWrap.WrapReverse
' COVER: Zanna.GUI.Dock.Fill
' COVER: Zanna.GUI.ThemeMode.Custom
' COVER: Zanna.GUI.AccessibleRole.Link
' COVER: Zanna.GUI.LiveRegionMode.Assertive
' COVER: Zanna.GUI.DialogButtonRole.Help
' COVER: Zanna.GUI.DialogStatus.Failed
' COVER: Zanna.GUI.ImageFilter.Bilinear
' COVER: Zanna.GUI.SortDirection.Descending

'=============================================================================
' Test typed GUI constant properties without constructing a GUI App
'=============================================================================
Zanna.Core.Diagnostics.AssertEq(Zanna.GUI.Align.Start, 0, "GUI.Align.Start")
Zanna.Core.Diagnostics.AssertEq(Zanna.GUI.Justify.SpaceEvenly, 5, "GUI.Justify.SpaceEvenly")
Zanna.Core.Diagnostics.AssertEq(Zanna.GUI.FlexDirection.Column, 1, "GUI.FlexDirection.Column")
Zanna.Core.Diagnostics.AssertEq(Zanna.GUI.FlexWrap.WrapReverse, 2, "GUI.FlexWrap.WrapReverse")
Zanna.Core.Diagnostics.AssertEq(Zanna.GUI.Dock.Fill, 4, "GUI.Dock.Fill")
Zanna.Core.Diagnostics.AssertEq(Zanna.GUI.ThemeMode.Custom, 3, "GUI.ThemeMode.Custom")
Zanna.Core.Diagnostics.AssertEq(Zanna.GUI.AccessibleRole.Link, 30, "GUI.AccessibleRole.Link")
Zanna.Core.Diagnostics.AssertEq(Zanna.GUI.LiveRegionMode.Assertive, 2, "GUI.LiveRegion.Assertive")
Zanna.Core.Diagnostics.AssertEq(Zanna.GUI.DialogButtonRole.Help, 6, "GUI.DialogButtonRole.Help")
Zanna.Core.Diagnostics.AssertEq(Zanna.GUI.DialogStatus.Failed, 4, "GUI.DialogStatus.Failed")
Zanna.Core.Diagnostics.AssertEq(Zanna.GUI.ImageFilter.Bilinear, 1, "GUI.ImageFilter.Bilinear")
Zanna.Core.Diagnostics.AssertEq(Zanna.GUI.SortDirection.Descending, -1, "GUI.Sort.Descending")

'=============================================================================
' Test Color Extended Methods
'=============================================================================
DIM red AS INTEGER
DIM green AS INTEGER
DIM blue AS INTEGER
DIM white AS INTEGER
red = Zanna.Graphics.Color.Rgb(255, 0, 0)
green = Zanna.Graphics.Color.Rgb(0, 255, 0)
blue = Zanna.Graphics.Color.Rgb(0, 0, 255)
white = Zanna.Graphics.Color.Rgb(255, 255, 255)

' Test GetR, GetG, GetB, GetA
DIM r AS INTEGER
DIM g AS INTEGER
DIM b AS INTEGER
DIM a AS INTEGER
r = Zanna.Graphics.Color.GetRed(red)
g = Zanna.Graphics.Color.GetGreen(red)
b = Zanna.Graphics.Color.GetBlue(red)
a = Zanna.Graphics.Color.GetAlpha(red)
Zanna.Core.Diagnostics.AssertEq(r, 255, "Color.GetR red")
Zanna.Core.Diagnostics.AssertEq(g, 0, "Color.GetG red")
Zanna.Core.Diagnostics.AssertEq(b, 0, "Color.GetB red")
' RGB() returns 0x00RRGGBB, so alpha is 0
Zanna.Core.Diagnostics.AssertEq(a, 0, "Color.GetA RGB red")

' Test GetA with RGBA color
DIM blueWithAlpha AS INTEGER
blueWithAlpha = Zanna.Graphics.Color.Rgba(0, 0, 255, 128)
a = Zanna.Graphics.Color.GetAlpha(blueWithAlpha)
Zanna.Core.Diagnostics.AssertEq(a, 128, "Color.GetA RGBA")

r = Zanna.Graphics.Color.GetRed(green)
g = Zanna.Graphics.Color.GetGreen(green)
b = Zanna.Graphics.Color.GetBlue(green)
Zanna.Core.Diagnostics.AssertEq(r, 0, "Color.GetR green")
Zanna.Core.Diagnostics.AssertEq(g, 255, "Color.GetG green")
Zanna.Core.Diagnostics.AssertEq(b, 0, "Color.GetB green")

' Test FromHSL - red is at hue 0
DIM hslRed AS INTEGER
hslRed = Zanna.Graphics.Color.FromHsl(0, 100, 50)
Zanna.Core.Diagnostics.Assert(Zanna.Graphics.Color.GetRed(hslRed) > 200, "Color.FromHSL red R")
Zanna.Core.Diagnostics.Assert(Zanna.Graphics.Color.GetGreen(hslRed) < 50, "Color.FromHSL red G")
Zanna.Core.Diagnostics.Assert(Zanna.Graphics.Color.GetBlue(hslRed) < 50, "Color.FromHSL red B")

' Test Lerp
DIM lerped AS INTEGER
lerped = Zanna.Graphics.Color.Lerp(red, blue, 50)
Zanna.Core.Diagnostics.Assert(Zanna.Graphics.Color.GetRed(lerped) > 100, "Color.Lerp R mid")
Zanna.Core.Diagnostics.Assert(Zanna.Graphics.Color.GetBlue(lerped) > 100, "Color.Lerp B mid")

' Test Brighten and Darken
DIM bright AS INTEGER
DIM dark AS INTEGER
bright = Zanna.Graphics.Color.Brighten(red, 50)
dark = Zanna.Graphics.Color.Darken(red, 50)
Zanna.Core.Diagnostics.Assert(Zanna.Graphics.Color.GetRed(bright) = 255, "Color.Brighten")
Zanna.Core.Diagnostics.Assert(Zanna.Graphics.Color.GetRed(dark) < 200, "Color.Darken")

'=============================================================================
' Test Pixels Extended Methods
'=============================================================================
DIM pixels AS Zanna.Graphics.Pixels
pixels = NEW Zanna.Graphics.Pixels(8, 8)
pixels.FillColor(red)

' Test Invert
DIM inverted AS Zanna.Graphics.Pixels
inverted = pixels.Invert()
Zanna.Core.Diagnostics.AssertEq(inverted.Width, 8, "Pixels.Invert width")
Zanna.Core.Diagnostics.AssertEq(inverted.Height, 8, "Pixels.Invert height")
' Red (255,0,0) inverted should be cyan (0,255,255)
DIM invColor AS INTEGER
invColor = inverted.GetColor(0, 0)
Zanna.Core.Diagnostics.AssertEq(Zanna.Graphics.Color.GetRed(invColor), 0, "Pixels.Invert R")
Zanna.Core.Diagnostics.AssertEq(Zanna.Graphics.Color.GetGreen(invColor), 255, "Pixels.Invert G")
Zanna.Core.Diagnostics.AssertEq(Zanna.Graphics.Color.GetBlue(invColor), 255, "Pixels.Invert B")

' Test Grayscale
DIM gray AS Zanna.Graphics.Pixels
gray = pixels.Grayscale()
Zanna.Core.Diagnostics.AssertEq(gray.Width, 8, "Pixels.Grayscale width")
DIM grayColor AS INTEGER
grayColor = gray.GetColor(0, 0)
' Grayscale of red should have R=G=B
DIM grayR AS INTEGER
DIM grayG AS INTEGER
grayR = Zanna.Graphics.Color.GetRed(grayColor)
grayG = Zanna.Graphics.Color.GetGreen(grayColor)
Zanna.Core.Diagnostics.AssertEq(grayR, grayG, "Pixels.Grayscale R=G")

' Test Tint
DIM tinted AS Zanna.Graphics.Pixels
DIM tintBlue AS INTEGER
tintBlue = Zanna.Graphics.Color.Rgba(0, 0, 255, 128)
tinted = pixels.Tint(tintBlue)
Zanna.Core.Diagnostics.AssertEq(tinted.Width, 8, "Pixels.Tint width")

' Test Blur
DIM blurred AS Zanna.Graphics.Pixels
blurred = pixels.Blur(1)
Zanna.Core.Diagnostics.AssertEq(blurred.Width, 8, "Pixels.Blur width")

' Test Resize
DIM resized AS Zanna.Graphics.Pixels
resized = pixels.Resize(16, 16)
Zanna.Core.Diagnostics.AssertEq(resized.Width, 16, "Pixels.Resize width")
Zanna.Core.Diagnostics.AssertEq(resized.Height, 16, "Pixels.Resize height")

'=============================================================================
' Test Canvas Extended Methods (GradientH, GradientV)
'=============================================================================
DIM canvas AS Zanna.Graphics.Canvas
canvas = NEW Zanna.Graphics.Canvas("Extended Test", 64, 48)

canvas.Clear(white)
canvas.GradientH(0, 0, 32, 16, red, blue)
canvas.GradientV(32, 0, 32, 16, green, red)
canvas.Flip()

'=============================================================================
' Test Sprite Class
'=============================================================================
DIM spritePixels AS Zanna.Graphics.Pixels
spritePixels = NEW Zanna.Graphics.Pixels(16, 16)
spritePixels.FillColor(red)

DIM sprite AS Zanna.Graphics.Sprite
sprite = NEW Zanna.Graphics.Sprite(spritePixels)

Zanna.Core.Diagnostics.AssertEq(sprite.X, 0, "Sprite.X initial")
Zanna.Core.Diagnostics.AssertEq(sprite.Y, 0, "Sprite.Y initial")
Zanna.Core.Diagnostics.AssertEq(sprite.Width, 16, "Sprite.Width")
Zanna.Core.Diagnostics.AssertEq(sprite.Height, 16, "Sprite.Height")
Zanna.Core.Diagnostics.Assert(sprite.Visible, "Sprite.Visible initial")
Zanna.Core.Diagnostics.AssertEq(sprite.ScaleX, 100, "Sprite.ScaleX initial")
Zanna.Core.Diagnostics.AssertEq(sprite.ScaleY, 100, "Sprite.ScaleY initial")
Zanna.Core.Diagnostics.AssertEq(sprite.FrameCount, 1, "Sprite.FrameCount")
Zanna.Core.Diagnostics.AssertEq(sprite.Frame, 0, "Sprite.Frame initial")

' Test property setters
sprite.X = 10
sprite.Y = 20
Zanna.Core.Diagnostics.AssertEq(sprite.X, 10, "Sprite.X set")
Zanna.Core.Diagnostics.AssertEq(sprite.Y, 20, "Sprite.Y set")

sprite.ScaleX = 200
sprite.ScaleY = 150
Zanna.Core.Diagnostics.AssertEq(sprite.ScaleX, 200, "Sprite.ScaleX set")
Zanna.Core.Diagnostics.AssertEq(sprite.ScaleY, 150, "Sprite.ScaleY set")

sprite.Visible = FALSE
Zanna.Core.Diagnostics.Assert(sprite.Visible = FALSE, "Sprite.Visible set")
sprite.Visible = TRUE

' Test Move
sprite.Move(5, 5)
Zanna.Core.Diagnostics.AssertEq(sprite.X, 15, "Sprite.Move X")
Zanna.Core.Diagnostics.AssertEq(sprite.Y, 25, "Sprite.Move Y")

' Test Contains
Zanna.Core.Diagnostics.Assert(sprite.Contains(15, 25), "Sprite.Contains inside")
Zanna.Core.Diagnostics.Assert(NOT sprite.Contains(0, 0), "Sprite.Contains outside")

' Test Draw
sprite.Draw(canvas)

'=============================================================================
' Test Tilemap Class
'=============================================================================
DIM tilemap AS Zanna.Graphics2D.Tilemap
tilemap = NEW Zanna.Graphics2D.Tilemap(10, 8, 16, 16)

Zanna.Core.Diagnostics.AssertEq(tilemap.Width, 10, "Tilemap.Width")
Zanna.Core.Diagnostics.AssertEq(tilemap.Height, 8, "Tilemap.Height")
Zanna.Core.Diagnostics.AssertEq(tilemap.TileWidth, 16, "Tilemap.TileWidth")
Zanna.Core.Diagnostics.AssertEq(tilemap.TileHeight, 16, "Tilemap.TileHeight")

' Test SetTile and GetTile
tilemap.SetTile(0, 0, 1)
tilemap.SetTile(1, 0, 2)
tilemap.SetTile(2, 0, 3)
Zanna.Core.Diagnostics.AssertEq(tilemap.GetTile(0, 0), 1, "Tilemap.GetTile 0,0")
Zanna.Core.Diagnostics.AssertEq(tilemap.GetTile(1, 0), 2, "Tilemap.GetTile 1,0")
Zanna.Core.Diagnostics.AssertEq(tilemap.GetTile(2, 0), 3, "Tilemap.GetTile 2,0")

' Test Fill
tilemap.Fill(5)
Zanna.Core.Diagnostics.AssertEq(tilemap.GetTile(0, 0), 5, "Tilemap.Fill 0,0")
Zanna.Core.Diagnostics.AssertEq(tilemap.GetTile(9, 7), 5, "Tilemap.Fill 9,7")

' Test Clear
tilemap.Clear()
Zanna.Core.Diagnostics.AssertEq(tilemap.GetTile(0, 0), 0, "Tilemap.Clear 0,0")

'=============================================================================
' Test Camera Class
'=============================================================================
DIM camera AS Zanna.Graphics.Camera
camera = NEW Zanna.Graphics.Camera(320, 240)

Zanna.Core.Diagnostics.AssertEq(camera.Width, 320, "Camera.Width")
Zanna.Core.Diagnostics.AssertEq(camera.Height, 240, "Camera.Height")
Zanna.Core.Diagnostics.AssertEq(camera.X, 0, "Camera.X initial")
Zanna.Core.Diagnostics.AssertEq(camera.Y, 0, "Camera.Y initial")
Zanna.Core.Diagnostics.AssertEq(camera.Zoom, 100, "Camera.Zoom initial")

' Test property setters
camera.X = 100
camera.Y = 50
Zanna.Core.Diagnostics.AssertEq(camera.X, 100, "Camera.X set")
Zanna.Core.Diagnostics.AssertEq(camera.Y, 50, "Camera.Y set")

camera.Zoom = 200
Zanna.Core.Diagnostics.AssertEq(camera.Zoom, 200, "Camera.Zoom set")

' Test Move
camera.Move(10, 20)
Zanna.Core.Diagnostics.AssertEq(camera.X, 110, "Camera.Move X")
Zanna.Core.Diagnostics.AssertEq(camera.Y, 70, "Camera.Move Y")

' Test coordinate transforms
' At zoom 200%, camera at (110, 70), viewport 320x240
' Screen center is at world (110 + 320/4, 70 + 240/4) = (190, 130)
' Screen (0, 0) should be at world (110, 70) approximately
DIM worldX AS INTEGER
DIM worldY AS INTEGER
DIM screenX AS INTEGER
DIM screenY AS INTEGER

worldX = camera.ToWorldX(0)
worldY = camera.ToWorldY(0)
screenX = camera.ToScreenX(110)
screenY = camera.ToScreenY(70)

' At 200% zoom, screen coords transform to world coords
Zanna.Core.Diagnostics.Assert(worldX >= 0, "Camera.ToWorldX valid")
Zanna.Core.Diagnostics.Assert(worldY >= 0, "Camera.ToWorldY valid")

PRINT "RESULT: ok"
END
