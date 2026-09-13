' =============================================================================
' API Audit: Zanna.Math.Mat4 - 4x4 Matrix Operations (BASIC)
' =============================================================================
' Tests: Identity, Zero, Translate, Scale, RotateX, RotateY, RotateZ, Get,
'        Add, Sub, Mul, MulScalar, TransformPoint, TransformVec, Transpose,
'        Det, Inverse, Perspective, Ortho, LookAt
' =============================================================================

PRINT "=== API Audit: Zanna.Math.Mat4 ==="

' --- Identity ---
PRINT "--- Identity ---"
DIM id AS OBJECT
id = Zanna.Math.Mat4.Identity()
PRINT "Identity [0,0]: "; Zanna.Math.Mat4.Get(id, 0, 0)
PRINT "Identity [1,1]: "; Zanna.Math.Mat4.Get(id, 1, 1)
PRINT "Identity [2,2]: "; Zanna.Math.Mat4.Get(id, 2, 2)
PRINT "Identity [3,3]: "; Zanna.Math.Mat4.Get(id, 3, 3)
PRINT "Identity [0,1]: "; Zanna.Math.Mat4.Get(id, 0, 1)

' --- Zero ---
PRINT "--- Zero ---"
DIM zr AS OBJECT
zr = Zanna.Math.Mat4.Zero()
PRINT "Zero [0,0]: "; Zanna.Math.Mat4.Get(zr, 0, 0)
PRINT "Zero [1,1]: "; Zanna.Math.Mat4.Get(zr, 1, 1)
PRINT "Zero [3,3]: "; Zanna.Math.Mat4.Get(zr, 3, 3)

' --- Translate ---
PRINT "--- Translate ---"
DIM tr AS OBJECT
tr = Zanna.Math.Mat4.Translate(1.0, 2.0, 3.0)
PRINT "Translate(1,2,3) [0,3]: "; Zanna.Math.Mat4.Get(tr, 0, 3)
PRINT "Translate(1,2,3) [1,3]: "; Zanna.Math.Mat4.Get(tr, 1, 3)
PRINT "Translate(1,2,3) [2,3]: "; Zanna.Math.Mat4.Get(tr, 2, 3)
PRINT "Translate(1,2,3) [0,0]: "; Zanna.Math.Mat4.Get(tr, 0, 0)

' --- Scale ---
PRINT "--- Scale ---"
DIM sc AS OBJECT
sc = Zanna.Math.Mat4.Scale(2.0, 3.0, 4.0)
PRINT "Scale(2,3,4) [0,0]: "; Zanna.Math.Mat4.Get(sc, 0, 0)
PRINT "Scale(2,3,4) [1,1]: "; Zanna.Math.Mat4.Get(sc, 1, 1)
PRINT "Scale(2,3,4) [2,2]: "; Zanna.Math.Mat4.Get(sc, 2, 2)
PRINT "Scale(2,3,4) [3,3]: "; Zanna.Math.Mat4.Get(sc, 3, 3)

' --- RotateX ---
PRINT "--- RotateX ---"
DIM rx AS OBJECT
rx = Zanna.Math.Mat4.RotateX(1.5707963267948966)
PRINT "RotateX(Pi/2) [1,1]: "; Zanna.Math.Mat4.Get(rx, 1, 1)
PRINT "RotateX(Pi/2) [1,2]: "; Zanna.Math.Mat4.Get(rx, 1, 2)
PRINT "RotateX(Pi/2) [2,1]: "; Zanna.Math.Mat4.Get(rx, 2, 1)
PRINT "RotateX(Pi/2) [2,2]: "; Zanna.Math.Mat4.Get(rx, 2, 2)

' --- RotateY ---
PRINT "--- RotateY ---"
DIM ry AS OBJECT
ry = Zanna.Math.Mat4.RotateY(1.5707963267948966)
PRINT "RotateY(Pi/2) [0,0]: "; Zanna.Math.Mat4.Get(ry, 0, 0)
PRINT "RotateY(Pi/2) [0,2]: "; Zanna.Math.Mat4.Get(ry, 0, 2)
PRINT "RotateY(Pi/2) [2,0]: "; Zanna.Math.Mat4.Get(ry, 2, 0)
PRINT "RotateY(Pi/2) [2,2]: "; Zanna.Math.Mat4.Get(ry, 2, 2)

' --- RotateZ ---
PRINT "--- RotateZ ---"
DIM rz AS OBJECT
rz = Zanna.Math.Mat4.RotateZ(1.5707963267948966)
PRINT "RotateZ(Pi/2) [0,0]: "; Zanna.Math.Mat4.Get(rz, 0, 0)
PRINT "RotateZ(Pi/2) [0,1]: "; Zanna.Math.Mat4.Get(rz, 0, 1)
PRINT "RotateZ(Pi/2) [1,0]: "; Zanna.Math.Mat4.Get(rz, 1, 0)
PRINT "RotateZ(Pi/2) [1,1]: "; Zanna.Math.Mat4.Get(rz, 1, 1)

' --- Add ---
PRINT "--- Add ---"
DIM msum AS OBJECT
msum = Zanna.Math.Mat4.Add(id, id)
PRINT "Identity + Identity [0,0]: "; Zanna.Math.Mat4.Get(msum, 0, 0)
PRINT "Identity + Identity [1,1]: "; Zanna.Math.Mat4.Get(msum, 1, 1)
PRINT "Identity + Identity [0,1]: "; Zanna.Math.Mat4.Get(msum, 0, 1)

' --- Sub ---
PRINT "--- Sub ---"
DIM mdiff AS OBJECT
mdiff = Zanna.Math.Mat4.Sub(id, id)
PRINT "Identity - Identity [0,0]: "; Zanna.Math.Mat4.Get(mdiff, 0, 0)
PRINT "Identity - Identity [1,1]: "; Zanna.Math.Mat4.Get(mdiff, 1, 1)

' --- Mul ---
PRINT "--- Mul ---"
DIM prod AS OBJECT
prod = Zanna.Math.Mat4.Mul(sc, tr)
PRINT "Scale * Translate [0,3]: "; Zanna.Math.Mat4.Get(prod, 0, 3)
PRINT "Scale * Translate [1,3]: "; Zanna.Math.Mat4.Get(prod, 1, 3)
PRINT "Scale * Translate [2,3]: "; Zanna.Math.Mat4.Get(prod, 2, 3)
DIM prodId AS OBJECT
prodId = Zanna.Math.Mat4.Mul(id, sc)
PRINT "Identity * Scale [0,0]: "; Zanna.Math.Mat4.Get(prodId, 0, 0)

' --- MulScalar ---
PRINT "--- MulScalar ---"
DIM ms AS OBJECT
ms = Zanna.Math.Mat4.MulScalar(id, 5.0)
PRINT "Identity * 5.0 [0,0]: "; Zanna.Math.Mat4.Get(ms, 0, 0)
PRINT "Identity * 5.0 [0,1]: "; Zanna.Math.Mat4.Get(ms, 0, 1)

' --- TransformPoint ---
PRINT "--- TransformPoint ---"
DIM pt AS Zanna.Math.Vec3
pt = Zanna.Math.Vec3.New(1.0, 0.0, 0.0)
DIM tpt AS Zanna.Math.Vec3
tpt = Zanna.Math.Mat4.TransformPoint(tr, pt)
PRINT "Translate(1,2,3) * Point(1,0,0) X: "; tpt.X
PRINT "Translate(1,2,3) * Point(1,0,0) Y: "; tpt.Y
PRINT "Translate(1,2,3) * Point(1,0,0) Z: "; tpt.Z

' --- TransformVec ---
PRINT "--- TransformVec ---"
DIM tv AS Zanna.Math.Vec3
tv = Zanna.Math.Mat4.TransformVector(tr, pt)
PRINT "Translate(1,2,3) * Vec(1,0,0) X: "; tv.X
PRINT "Translate(1,2,3) * Vec(1,0,0) Y: "; tv.Y
PRINT "Translate(1,2,3) * Vec(1,0,0) Z: "; tv.Z

' --- Transpose ---
PRINT "--- Transpose ---"
DIM tt AS OBJECT
tt = Zanna.Math.Mat4.Transpose(tr)
PRINT "Transpose(Translate) [3,0]: "; Zanna.Math.Mat4.Get(tt, 3, 0)
PRINT "Transpose(Translate) [3,1]: "; Zanna.Math.Mat4.Get(tt, 3, 1)
PRINT "Transpose(Translate) [3,2]: "; Zanna.Math.Mat4.Get(tt, 3, 2)

' --- Det ---
PRINT "--- Det ---"
PRINT "Det(Identity): "; Zanna.Math.Mat4.Determinant(id)
PRINT "Det(Scale(2,3,4)): "; Zanna.Math.Mat4.Determinant(sc)

' --- Inverse ---
PRINT "--- Inverse ---"
DIM inv AS OBJECT
inv = Zanna.Math.Mat4.Inverse(sc)
PRINT "Inverse(Scale(2,3,4)) [0,0]: "; Zanna.Math.Mat4.Get(inv, 0, 0)
PRINT "Inverse(Scale(2,3,4)) [1,1]: "; Zanna.Math.Mat4.Get(inv, 1, 1)
PRINT "Inverse(Scale(2,3,4)) [2,2]: "; Zanna.Math.Mat4.Get(inv, 2, 2)
DIM invId AS OBJECT
invId = Zanna.Math.Mat4.Inverse(id)
PRINT "Inverse(Identity) [0,0]: "; Zanna.Math.Mat4.Get(invId, 0, 0)

' --- Perspective ---
PRINT "--- Perspective ---"
DIM persp AS OBJECT
persp = Zanna.Math.Mat4.Perspective(1.0471975511965976, 1.7777777777777777, 0.1, 100.0)
PRINT "Perspective(60deg, 16:9, 0.1, 100) [0,0]: "; Zanna.Math.Mat4.Get(persp, 0, 0)
PRINT "Perspective [1,1]: "; Zanna.Math.Mat4.Get(persp, 1, 1)
PRINT "Perspective [2,2]: "; Zanna.Math.Mat4.Get(persp, 2, 2)
PRINT "Perspective [3,2]: "; Zanna.Math.Mat4.Get(persp, 3, 2)

' --- Ortho ---
PRINT "--- Ortho ---"
DIM ortho AS OBJECT
ortho = Zanna.Math.Mat4.Orthographic(-1.0, 1.0, -1.0, 1.0, 0.1, 100.0)
PRINT "Ortho(-1,1,-1,1,0.1,100) [0,0]: "; Zanna.Math.Mat4.Get(ortho, 0, 0)
PRINT "Ortho [1,1]: "; Zanna.Math.Mat4.Get(ortho, 1, 1)
PRINT "Ortho [2,2]: "; Zanna.Math.Mat4.Get(ortho, 2, 2)

' --- LookAt ---
PRINT "--- LookAt ---"
DIM eye AS Zanna.Math.Vec3
eye = Zanna.Math.Vec3.New(0.0, 0.0, 5.0)
DIM center AS Zanna.Math.Vec3
center = Zanna.Math.Vec3.New(0.0, 0.0, 0.0)
DIM up AS Zanna.Math.Vec3
up = Zanna.Math.Vec3.New(0.0, 1.0, 0.0)
DIM la AS OBJECT
la = Zanna.Math.Mat4.LookAt(eye, center, up)
PRINT "LookAt(eye=(0,0,5), center=origin, up=(0,1,0)) [0,0]: "; Zanna.Math.Mat4.Get(la, 0, 0)
PRINT "LookAt [1,1]: "; Zanna.Math.Mat4.Get(la, 1, 1)
PRINT "LookAt [2,3]: "; Zanna.Math.Mat4.Get(la, 2, 3)

PRINT "=== Mat4 Audit Complete ==="
END
