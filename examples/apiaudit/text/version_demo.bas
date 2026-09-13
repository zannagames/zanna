' Zanna.Text.Version API Audit - Semantic Versioning
' Tests all Version functions

PRINT "=== Zanna.Text.Version API Audit ==="

' --- Parse ---
PRINT "--- Parse ---"
DIM v1 AS OBJECT
DIM v2 AS OBJECT
v1 = Zanna.Text.Version.Parse("1.2.3")
v2 = Zanna.Text.Version.Parse("2.0.0-alpha.1+build.42")

' --- IsValid ---
PRINT "--- IsValid ---"
PRINT "1.2.3: "; Zanna.Text.Version.IsValid("1.2.3")
PRINT "2.0.0-alpha.1: "; Zanna.Text.Version.IsValid("2.0.0-alpha.1")
PRINT "not-a-version: "; Zanna.Text.Version.IsValid("not-a-version")
PRINT "1.2: "; Zanna.Text.Version.IsValid("1.2")
PRINT "0.0.0: "; Zanna.Text.Version.IsValid("0.0.0")

' --- get_Major / get_Minor / get_Patch ---
PRINT "--- get_Major / get_Minor / get_Patch ---"
PRINT "v1 major: "; Zanna.Text.Version.get_Major(v1)
PRINT "v1 minor: "; Zanna.Text.Version.get_Minor(v1)
PRINT "v1 patch: "; Zanna.Text.Version.get_Patch(v1)
PRINT "v2 major: "; Zanna.Text.Version.get_Major(v2)
PRINT "v2 minor: "; Zanna.Text.Version.get_Minor(v2)
PRINT "v2 patch: "; Zanna.Text.Version.get_Patch(v2)

' --- get_Prerelease ---
PRINT "--- get_Prerelease ---"
PRINT "v1 prerelease: "; Zanna.Text.Version.get_Prerelease(v1)
PRINT "v2 prerelease: "; Zanna.Text.Version.get_Prerelease(v2)

' --- get_Build ---
PRINT "--- get_Build ---"
PRINT "v1 build: "; Zanna.Text.Version.get_Build(v1)
PRINT "v2 build: "; Zanna.Text.Version.get_Build(v2)

' --- ToString ---
PRINT "--- ToString ---"
PRINT Zanna.Text.Version.ToString(v1)
PRINT Zanna.Text.Version.ToString(v2)

' --- Cmp ---
PRINT "--- Cmp ---"
DIM v3 AS OBJECT
DIM v4 AS OBJECT
DIM v5 AS OBJECT
v3 = Zanna.Text.Version.Parse("1.0.0")
v4 = Zanna.Text.Version.Parse("2.0.0")
v5 = Zanna.Text.Version.Parse("1.0.0")
PRINT "1.0.0 vs 2.0.0: "; v3.CompareTo(v4)
PRINT "2.0.0 vs 1.0.0: "; v4.CompareTo(v3)
PRINT "1.0.0 vs 1.0.0: "; v3.CompareTo(v5)

' --- Satisfies ---
PRINT "--- Satisfies ---"
DIM v6 AS OBJECT
v6 = Zanna.Text.Version.Parse("1.5.3")
PRINT "1.5.3 >= 1.0.0: "; Zanna.Text.Version.Satisfies(v6, ">=1.0.0")
PRINT "1.5.3 >= 2.0.0: "; Zanna.Text.Version.Satisfies(v6, ">=2.0.0")

' --- BumpMajor / BumpMinor / BumpPatch ---
PRINT "--- BumpMajor / BumpMinor / BumpPatch ---"
PRINT "BumpMajor 1.2.3: "; Zanna.Text.Version.BumpMajor(v1)
PRINT "BumpMinor 1.2.3: "; Zanna.Text.Version.BumpMinor(v1)
PRINT "BumpPatch 1.2.3: "; Zanna.Text.Version.BumpPatch(v1)

DIM v7 AS OBJECT
v7 = Zanna.Text.Version.Parse("0.9.9")
PRINT "BumpMajor 0.9.9: "; Zanna.Text.Version.BumpMajor(v7)
PRINT "BumpMinor 0.9.9: "; Zanna.Text.Version.BumpMinor(v7)
PRINT "BumpPatch 0.9.9: "; Zanna.Text.Version.BumpPatch(v7)

PRINT "=== Version Demo Complete ==="
END
