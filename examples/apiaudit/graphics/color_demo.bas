' =============================================================================
' API Audit: Zanna.Graphics.Color (BASIC)
' =============================================================================
' Tests: RGB, RGBA, FromHSL, GetRed, GetGreen, GetBlue, GetAlpha, GetHue, GetSaturation, GetLightness,
'        Lerp, Brighten, Darken, FromHex, ToHex, Saturate, Desaturate,
'        Complement, Grayscale, Invert
' =============================================================================

PRINT "=== API Audit: Zanna.Graphics.Color ==="

' --- RGB ---
PRINT "--- RGB ---"
DIM red AS INTEGER = Zanna.Graphics.Color.RGB(255, 0, 0)
PRINT "RGB(255, 0, 0): "; red
DIM green AS INTEGER = Zanna.Graphics.Color.RGB(0, 255, 0)
PRINT "RGB(0, 255, 0): "; green
DIM blue AS INTEGER = Zanna.Graphics.Color.RGB(0, 0, 255)
PRINT "RGB(0, 0, 255): "; blue
DIM white AS INTEGER = Zanna.Graphics.Color.RGB(255, 255, 255)
PRINT "RGB(255, 255, 255): "; white
DIM black AS INTEGER = Zanna.Graphics.Color.RGB(0, 0, 0)
PRINT "RGB(0, 0, 0): "; black

' --- RGBA ---
PRINT "--- RGBA ---"
DIM semiRed AS INTEGER = Zanna.Graphics.Color.RGBA(255, 0, 0, 128)
PRINT "RGBA(255, 0, 0, 128): "; semiRed
DIM opaqueBlue AS INTEGER = Zanna.Graphics.Color.RGBA(0, 0, 255, 255)
PRINT "RGBA(0, 0, 255, 255): "; opaqueBlue
DIM transparent AS INTEGER = Zanna.Graphics.Color.RGBA(0, 0, 0, 0)
PRINT "RGBA(0, 0, 0, 0): "; transparent

' --- GetRed ---
PRINT "--- GetRed ---"
PRINT "GetRed(red): "; Zanna.Graphics.Color.GetRed(red)
PRINT "GetRed(green): "; Zanna.Graphics.Color.GetRed(green)
PRINT "GetRed(white): "; Zanna.Graphics.Color.GetRed(white)

' --- GetGreen ---
PRINT "--- GetGreen ---"
PRINT "GetGreen(red): "; Zanna.Graphics.Color.GetGreen(red)
PRINT "GetGreen(green): "; Zanna.Graphics.Color.GetGreen(green)
PRINT "GetGreen(white): "; Zanna.Graphics.Color.GetGreen(white)

' --- GetBlue ---
PRINT "--- GetBlue ---"
PRINT "GetBlue(red): "; Zanna.Graphics.Color.GetBlue(red)
PRINT "GetBlue(blue): "; Zanna.Graphics.Color.GetBlue(blue)
PRINT "GetBlue(white): "; Zanna.Graphics.Color.GetBlue(white)

' --- GetAlpha ---
PRINT "--- GetAlpha ---"
PRINT "GetAlpha(semiRed): "; Zanna.Graphics.Color.GetAlpha(semiRed)
PRINT "GetAlpha(opaqueBlue): "; Zanna.Graphics.Color.GetAlpha(opaqueBlue)
PRINT "GetAlpha(transparent): "; Zanna.Graphics.Color.GetAlpha(transparent)

' --- FromHSL ---
PRINT "--- FromHSL ---"
DIM hslRed AS INTEGER = Zanna.Graphics.Color.FromHsl(0, 100, 50)
PRINT "FromHSL(0, 100, 50): "; hslRed
DIM hslGreen AS INTEGER = Zanna.Graphics.Color.FromHsl(120, 100, 50)
PRINT "FromHSL(120, 100, 50): "; hslGreen
DIM hslBlue AS INTEGER = Zanna.Graphics.Color.FromHsl(240, 100, 50)
PRINT "FromHSL(240, 100, 50): "; hslBlue
DIM hslWhite AS INTEGER = Zanna.Graphics.Color.FromHsl(0, 0, 100)
PRINT "FromHSL(0, 0, 100): "; hslWhite
DIM hslBlack AS INTEGER = Zanna.Graphics.Color.FromHsl(0, 0, 0)
PRINT "FromHSL(0, 0, 0): "; hslBlack

' --- GetHue ---
PRINT "--- GetHue ---"
PRINT "GetHue(red): "; Zanna.Graphics.Color.GetHue(red)
PRINT "GetHue(green): "; Zanna.Graphics.Color.GetHue(green)
PRINT "GetHue(blue): "; Zanna.Graphics.Color.GetHue(blue)

' --- GetSaturation ---
PRINT "--- GetSaturation ---"
PRINT "GetSaturation(red): "; Zanna.Graphics.Color.GetSaturation(red)
PRINT "GetSaturation(white): "; Zanna.Graphics.Color.GetSaturation(white)

' --- GetLightness ---
PRINT "--- GetLightness ---"
PRINT "GetLightness(red): "; Zanna.Graphics.Color.GetLightness(red)
PRINT "GetLightness(white): "; Zanna.Graphics.Color.GetLightness(white)
PRINT "GetLightness(black): "; Zanna.Graphics.Color.GetLightness(black)

' --- Lerp ---
PRINT "--- Lerp ---"
DIM lerp0 AS INTEGER = Zanna.Graphics.Color.Lerp(red, blue, 0)
PRINT "Lerp(red, blue, 0): "; lerp0
DIM lerp50 AS INTEGER = Zanna.Graphics.Color.Lerp(red, blue, 50)
PRINT "Lerp(red, blue, 50): "; lerp50
DIM lerp100 AS INTEGER = Zanna.Graphics.Color.Lerp(red, blue, 100)
PRINT "Lerp(red, blue, 100): "; lerp100
PRINT "Lerp R at 50%: "; Zanna.Graphics.Color.GetRed(lerp50)
PRINT "Lerp B at 50%: "; Zanna.Graphics.Color.GetBlue(lerp50)

' --- Brighten ---
PRINT "--- Brighten ---"
DIM brightRed AS INTEGER = Zanna.Graphics.Color.Brighten(red, 30)
PRINT "Brighten(red, 30): "; brightRed
DIM brightBlue AS INTEGER = Zanna.Graphics.Color.Brighten(blue, 50)
PRINT "Brighten(blue, 50): "; brightBlue

' --- Darken ---
PRINT "--- Darken ---"
DIM darkRed AS INTEGER = Zanna.Graphics.Color.Darken(red, 30)
PRINT "Darken(red, 30): "; darkRed
DIM darkWhite AS INTEGER = Zanna.Graphics.Color.Darken(white, 50)
PRINT "Darken(white, 50): "; darkWhite

' --- FromHex ---
PRINT "--- FromHex ---"
DIM hexRed AS INTEGER = Zanna.Graphics.Color.FromHex("#FF0000")
PRINT "FromHex(#FF0000): "; hexRed
DIM hexGreen AS INTEGER = Zanna.Graphics.Color.FromHex("#00FF00")
PRINT "FromHex(#00FF00): "; hexGreen
DIM hexBlue AS INTEGER = Zanna.Graphics.Color.FromHex("#0000FF")
PRINT "FromHex(#0000FF): "; hexBlue

' --- ToHex ---
PRINT "--- ToHex ---"
PRINT "ToHex(red): "; Zanna.Graphics.Color.ToHex(red)
PRINT "ToHex(green): "; Zanna.Graphics.Color.ToHex(green)
PRINT "ToHex(blue): "; Zanna.Graphics.Color.ToHex(blue)

' --- Saturate ---
PRINT "--- Saturate ---"
DIM muted AS INTEGER = Zanna.Graphics.Color.RGB(128, 100, 100)
DIM saturated AS INTEGER = Zanna.Graphics.Color.Saturate(muted, 50)
PRINT "Saturate(muted, 50): "; saturated

' --- Desaturate ---
PRINT "--- Desaturate ---"
DIM desaturated AS INTEGER = Zanna.Graphics.Color.Desaturate(red, 50)
PRINT "Desaturate(red, 50): "; desaturated
DIM fullDesat AS INTEGER = Zanna.Graphics.Color.Desaturate(red, 100)
PRINT "Desaturate(red, 100): "; fullDesat

' --- Complement ---
PRINT "--- Complement ---"
DIM compRed AS INTEGER = Zanna.Graphics.Color.Complement(red)
PRINT "Complement(red): "; compRed
PRINT "Complement R: "; Zanna.Graphics.Color.GetRed(compRed)
PRINT "Complement G: "; Zanna.Graphics.Color.GetGreen(compRed)
PRINT "Complement B: "; Zanna.Graphics.Color.GetBlue(compRed)

' --- Grayscale ---
PRINT "--- Grayscale ---"
DIM grayRed AS INTEGER = Zanna.Graphics.Color.Grayscale(red)
PRINT "Grayscale(red): "; grayRed
DIM grayGreen AS INTEGER = Zanna.Graphics.Color.Grayscale(green)
PRINT "Grayscale(green): "; grayGreen
DIM grayWhite AS INTEGER = Zanna.Graphics.Color.Grayscale(white)
PRINT "Grayscale(white): "; grayWhite

' --- Invert ---
PRINT "--- Invert ---"
DIM invRed AS INTEGER = Zanna.Graphics.Color.Invert(red)
PRINT "Invert(red): "; invRed
PRINT "Invert R: "; Zanna.Graphics.Color.GetRed(invRed)
PRINT "Invert G: "; Zanna.Graphics.Color.GetGreen(invRed)
PRINT "Invert B: "; Zanna.Graphics.Color.GetBlue(invRed)
DIM invBlack AS INTEGER = Zanna.Graphics.Color.Invert(black)
PRINT "Invert(black): "; invBlack
DIM invWhite AS INTEGER = Zanna.Graphics.Color.Invert(white)
PRINT "Invert(white): "; invWhite

PRINT "=== Color Audit Complete ==="
END
