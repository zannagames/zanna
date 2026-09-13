' test_json_csv.bas — Zanna.Data.Json + Csv + Uuid + Version
DIM j AS OBJECT
LET j = Zanna.Data.Json.Parse("{""name"":""zanna"",""ver"":1}")
PRINT Zanna.Collections.Map.GetStr(j, "name")
PRINT Zanna.Collections.Map.GetInt(j, "ver")
PRINT Zanna.Collections.Map.Has(j, "name")
PRINT Zanna.Collections.Map.Has(j, "missing")
DIM js AS STRING
LET js = Zanna.Data.Json.Format(j)
PRINT Zanna.String.Contains(js, "zanna")

DIM j2 AS OBJECT
LET j2 = Zanna.Data.Json.NewObject()
Zanna.Collections.Map.SetStr(j2, "key", "value")
Zanna.Collections.Map.SetInt(j2, "num", 42)
Zanna.Collections.Map.SetBool(j2, "flag", TRUE)
PRINT Zanna.Collections.Map.GetStr(j2, "key")
PRINT Zanna.Collections.Map.GetInt(j2, "num")
PRINT Zanna.Collections.Map.GetBool(j2, "flag")
PRINT Zanna.Data.Json.Format(j2)

' Uuid
DIM u1 AS STRING
LET u1 = Zanna.Text.Uuid.Generate()
PRINT Zanna.String.Contains(u1, "-")
DIM u2 AS STRING
LET u2 = Zanna.Text.Uuid.Generate()
PRINT u1 <> u2

' Version
PRINT Zanna.Text.Version.Parse("1.2.3")
PRINT Zanna.Text.Version.Compare("1.2.3", "1.3.0")
PRINT Zanna.Text.Version.Compare("2.0.0", "1.9.9")
PRINT Zanna.Text.Version.IsValid("1.2.3")
PRINT Zanna.Text.Version.IsValid("abc")
PRINT Zanna.Text.Version.ParseMajor("1.2.3")
PRINT Zanna.Text.Version.ParseMinor("1.2.3")
PRINT Zanna.Text.Version.ParsePatch("1.2.3")

PRINT "done"
END
