' =============================================================================
' API Audit: Zanna.Math.Vec2 - 2D Vector Operations (BASIC)
' =============================================================================
' Tests: New, Zero, One, X, Y, Add, Sub, Mul, Div, Neg, Dot, Cross,
'        Len, LenSq, Dist, Norm, Lerp, Angle, Rotate
' =============================================================================

PRINT "=== API Audit: Zanna.Math.Vec2 ==="

' --- Factory: New ---
PRINT "--- New ---"
DIM v1 AS Zanna.Math.Vec2
v1 = Zanna.Math.Vec2.New(3.0, 4.0)
PRINT "Vec2.New(3.0, 4.0) X: "; v1.X
PRINT "Vec2.New(3.0, 4.0) Y: "; v1.Y

' --- Factory: Zero ---
PRINT "--- Zero ---"
DIM vz AS Zanna.Math.Vec2
vz = Zanna.Math.Vec2.Zero()
PRINT "Vec2.Zero() X: "; vz.X
PRINT "Vec2.Zero() Y: "; vz.Y

' --- Factory: One ---
PRINT "--- One ---"
DIM vo AS Zanna.Math.Vec2
vo = Zanna.Math.Vec2.One()
PRINT "Vec2.One() X: "; vo.X
PRINT "Vec2.One() Y: "; vo.Y

' --- Add ---
PRINT "--- Add ---"
DIM v2 AS Zanna.Math.Vec2
v2 = Zanna.Math.Vec2.New(1.0, 2.0)
DIM vadd AS Zanna.Math.Vec2
vadd = v1.Add(v2)
PRINT "Add(3,4 + 1,2) X: "; vadd.X
PRINT "Add(3,4 + 1,2) Y: "; vadd.Y

' --- Sub ---
PRINT "--- Sub ---"
DIM vsub AS Zanna.Math.Vec2
vsub = v1.Sub(v2)
PRINT "Sub(3,4 - 1,2) X: "; vsub.X
PRINT "Sub(3,4 - 1,2) Y: "; vsub.Y

' --- Mul ---
PRINT "--- Mul ---"
DIM vmul AS Zanna.Math.Vec2
vmul = v1.Mul(2.0)
PRINT "Mul(3,4 * 2.0) X: "; vmul.X
PRINT "Mul(3,4 * 2.0) Y: "; vmul.Y

' --- Div ---
PRINT "--- Div ---"
DIM vdiv AS Zanna.Math.Vec2
vdiv = v1.Div(2.0)
PRINT "Div(3,4 / 2.0) X: "; vdiv.X
PRINT "Div(3,4 / 2.0) Y: "; vdiv.Y

' --- Neg ---
PRINT "--- Neg ---"
DIM vneg AS Zanna.Math.Vec2
vneg = v1.Negate()
PRINT "Neg(3,4) X: "; vneg.X
PRINT "Neg(3,4) Y: "; vneg.Y

' --- Dot ---
PRINT "--- Dot ---"
PRINT "Dot((3,4), (1,2)): "; v1.Dot(v2)
DIM vx AS Zanna.Math.Vec2
vx = Zanna.Math.Vec2.New(1.0, 0.0)
DIM vy AS Zanna.Math.Vec2
vy = Zanna.Math.Vec2.New(0.0, 1.0)
PRINT "Dot((1,0), (0,1)): "; vx.Dot(vy)

' --- Cross ---
PRINT "--- Cross ---"
PRINT "Cross((3,4), (1,2)): "; v1.Cross(v2)
PRINT "Cross((1,0), (0,1)): "; vx.Cross(vy)

' --- Len ---
PRINT "--- Len ---"
PRINT "Len(3,4): "; v1.Len()
PRINT "Len(0,0): "; vz.Len()

' --- LenSq ---
PRINT "--- LenSq ---"
PRINT "LenSq(3,4): "; v1.LengthSquared()

' --- Dist ---
PRINT "--- Dist ---"
PRINT "Dist((3,4), (0,0)): "; v1.Dist(vz)
PRINT "Dist((3,4), (3,4)): "; v1.Dist(v1)

' --- Norm ---
PRINT "--- Norm ---"
DIM vnorm AS Zanna.Math.Vec2
vnorm = v1.Norm()
PRINT "Norm(3,4) X: "; vnorm.X
PRINT "Norm(3,4) Y: "; vnorm.Y
PRINT "Norm(3,4) Len: "; vnorm.Len()

' --- Lerp ---
PRINT "--- Lerp ---"
DIM va AS Zanna.Math.Vec2
va = Zanna.Math.Vec2.New(0.0, 0.0)
DIM vb AS Zanna.Math.Vec2
vb = Zanna.Math.Vec2.New(10.0, 20.0)
DIM vl0 AS Zanna.Math.Vec2
vl0 = va.Lerp(vb, 0.0)
PRINT "Lerp(0,0 -> 10,20 t=0.0) X: "; vl0.X
PRINT "Lerp(0,0 -> 10,20 t=0.0) Y: "; vl0.Y
DIM vl5 AS Zanna.Math.Vec2
vl5 = va.Lerp(vb, 0.5)
PRINT "Lerp(0,0 -> 10,20 t=0.5) X: "; vl5.X
PRINT "Lerp(0,0 -> 10,20 t=0.5) Y: "; vl5.Y
DIM vl1 AS Zanna.Math.Vec2
vl1 = va.Lerp(vb, 1.0)
PRINT "Lerp(0,0 -> 10,20 t=1.0) X: "; vl1.X
PRINT "Lerp(0,0 -> 10,20 t=1.0) Y: "; vl1.Y

' --- Angle ---
PRINT "--- Angle ---"
PRINT "Angle(1,0): "; vx.Heading()
PRINT "Angle(0,1): "; vy.Heading()
PRINT "Angle(3,4): "; v1.Heading()

' --- Rotate ---
PRINT "--- Rotate ---"
DIM vrot AS Zanna.Math.Vec2
vrot = vx.Rotate(1.5707963267948966)
PRINT "Rotate((1,0), Pi/2) X: "; vrot.X
PRINT "Rotate((1,0), Pi/2) Y: "; vrot.Y
DIM vrot2 AS Zanna.Math.Vec2
vrot2 = vx.Rotate(3.14159265358979)
PRINT "Rotate((1,0), Pi) X: "; vrot2.X
PRINT "Rotate((1,0), Pi) Y: "; vrot2.Y

PRINT "=== Vec2 Audit Complete ==="
END
