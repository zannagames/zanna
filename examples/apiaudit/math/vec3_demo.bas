' =============================================================================
' API Audit: Zanna.Math.Vec3 - 3D Vector Operations (BASIC)
' =============================================================================
' Tests: New, Zero, One, X, Y, Z, Add, Sub, Mul, Div, Neg, Dot, Cross,
'        Len, LenSq, Dist, Norm, Lerp
' =============================================================================

PRINT "=== API Audit: Zanna.Math.Vec3 ==="

' --- Factory: New ---
PRINT "--- New ---"
DIM v1 AS Zanna.Math.Vec3
v1 = Zanna.Math.Vec3.New(1.0, 2.0, 3.0)
PRINT "Vec3.New(1.0, 2.0, 3.0) X: "; v1.X
PRINT "Vec3.New(1.0, 2.0, 3.0) Y: "; v1.Y
PRINT "Vec3.New(1.0, 2.0, 3.0) Z: "; v1.Z

' --- Factory: Zero ---
PRINT "--- Zero ---"
DIM vz AS Zanna.Math.Vec3
vz = Zanna.Math.Vec3.Zero()
PRINT "Vec3.Zero() X: "; vz.X
PRINT "Vec3.Zero() Y: "; vz.Y
PRINT "Vec3.Zero() Z: "; vz.Z

' --- Factory: One ---
PRINT "--- One ---"
DIM vo AS Zanna.Math.Vec3
vo = Zanna.Math.Vec3.One()
PRINT "Vec3.One() X: "; vo.X
PRINT "Vec3.One() Y: "; vo.Y
PRINT "Vec3.One() Z: "; vo.Z

' --- Add ---
PRINT "--- Add ---"
DIM v2 AS Zanna.Math.Vec3
v2 = Zanna.Math.Vec3.New(2.0, 0.0, 1.0)
DIM vadd AS Zanna.Math.Vec3
vadd = v1.Add(v2)
PRINT "Add((1,2,3) + (2,0,1)) X: "; vadd.X
PRINT "Add((1,2,3) + (2,0,1)) Y: "; vadd.Y
PRINT "Add((1,2,3) + (2,0,1)) Z: "; vadd.Z

' --- Sub ---
PRINT "--- Sub ---"
DIM vsub AS Zanna.Math.Vec3
vsub = v1.Sub(v2)
PRINT "Sub((1,2,3) - (2,0,1)) X: "; vsub.X
PRINT "Sub((1,2,3) - (2,0,1)) Y: "; vsub.Y
PRINT "Sub((1,2,3) - (2,0,1)) Z: "; vsub.Z

' --- Mul ---
PRINT "--- Mul ---"
DIM vmul AS Zanna.Math.Vec3
vmul = v1.Mul(3.0)
PRINT "Mul((1,2,3) * 3.0) X: "; vmul.X
PRINT "Mul((1,2,3) * 3.0) Y: "; vmul.Y
PRINT "Mul((1,2,3) * 3.0) Z: "; vmul.Z

' --- Div ---
PRINT "--- Div ---"
DIM vdiv AS Zanna.Math.Vec3
vdiv = v1.Div(2.0)
PRINT "Div((1,2,3) / 2.0) X: "; vdiv.X
PRINT "Div((1,2,3) / 2.0) Y: "; vdiv.Y
PRINT "Div((1,2,3) / 2.0) Z: "; vdiv.Z

' --- Neg ---
PRINT "--- Neg ---"
DIM vneg AS Zanna.Math.Vec3
vneg = v1.Negate()
PRINT "Neg(1,2,3) X: "; vneg.X
PRINT "Neg(1,2,3) Y: "; vneg.Y
PRINT "Neg(1,2,3) Z: "; vneg.Z

' --- Dot ---
PRINT "--- Dot ---"
PRINT "Dot((1,2,3), (2,0,1)): "; v1.Dot(v2)
DIM vxu AS Zanna.Math.Vec3
vxu = Zanna.Math.Vec3.New(1.0, 0.0, 0.0)
DIM vyu AS Zanna.Math.Vec3
vyu = Zanna.Math.Vec3.New(0.0, 1.0, 0.0)
PRINT "Dot((1,0,0), (0,1,0)): "; vxu.Dot(vyu)

' --- Cross ---
PRINT "--- Cross ---"
DIM vc AS Zanna.Math.Vec3
vc = v1.Cross(v2)
PRINT "Cross((1,2,3), (2,0,1)) X: "; vc.X
PRINT "Cross((1,2,3), (2,0,1)) Y: "; vc.Y
PRINT "Cross((1,2,3), (2,0,1)) Z: "; vc.Z
DIM vcxy AS Zanna.Math.Vec3
vcxy = vxu.Cross(vyu)
PRINT "Cross((1,0,0), (0,1,0)) X: "; vcxy.X
PRINT "Cross((1,0,0), (0,1,0)) Y: "; vcxy.Y
PRINT "Cross((1,0,0), (0,1,0)) Z: "; vcxy.Z

' --- Len ---
PRINT "--- Len ---"
PRINT "Len(1,2,3): "; v1.Len()
PRINT "Len(0,0,0): "; vz.Len()
DIM v34 AS Zanna.Math.Vec3
v34 = Zanna.Math.Vec3.New(3.0, 4.0, 0.0)
PRINT "Len(3,4,0): "; v34.Len()

' --- LenSq ---
PRINT "--- LenSq ---"
PRINT "LenSq(1,2,3): "; v1.LengthSquared()
PRINT "LenSq(3,4,0): "; v34.LengthSquared()

' --- Dist ---
PRINT "--- Dist ---"
PRINT "Dist((1,2,3), (0,0,0)): "; v1.Dist(vz)
PRINT "Dist((1,2,3), (1,2,3)): "; v1.Dist(v1)

' --- Norm ---
PRINT "--- Norm ---"
DIM vnorm AS Zanna.Math.Vec3
vnorm = v1.Norm()
PRINT "Norm(1,2,3) X: "; vnorm.X
PRINT "Norm(1,2,3) Y: "; vnorm.Y
PRINT "Norm(1,2,3) Z: "; vnorm.Z
PRINT "Norm(1,2,3) Len: "; vnorm.Len()

' --- Lerp ---
PRINT "--- Lerp ---"
DIM va AS Zanna.Math.Vec3
va = Zanna.Math.Vec3.New(0.0, 0.0, 0.0)
DIM vb AS Zanna.Math.Vec3
vb = Zanna.Math.Vec3.New(10.0, 20.0, 30.0)
DIM vl0 AS Zanna.Math.Vec3
vl0 = va.Lerp(vb, 0.0)
PRINT "Lerp(origin -> (10,20,30) t=0.0) X: "; vl0.X
PRINT "Lerp(origin -> (10,20,30) t=0.0) Y: "; vl0.Y
PRINT "Lerp(origin -> (10,20,30) t=0.0) Z: "; vl0.Z
DIM vl5 AS Zanna.Math.Vec3
vl5 = va.Lerp(vb, 0.5)
PRINT "Lerp(origin -> (10,20,30) t=0.5) X: "; vl5.X
PRINT "Lerp(origin -> (10,20,30) t=0.5) Y: "; vl5.Y
PRINT "Lerp(origin -> (10,20,30) t=0.5) Z: "; vl5.Z
DIM vl1 AS Zanna.Math.Vec3
vl1 = va.Lerp(vb, 1.0)
PRINT "Lerp(origin -> (10,20,30) t=1.0) X: "; vl1.X
PRINT "Lerp(origin -> (10,20,30) t=1.0) Y: "; vl1.Y
PRINT "Lerp(origin -> (10,20,30) t=1.0) Z: "; vl1.Z

PRINT "=== Vec3 Audit Complete ==="
END
