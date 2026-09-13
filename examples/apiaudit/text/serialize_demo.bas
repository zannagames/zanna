' =============================================================================
' API Audit: Zanna.Data.Serialize - Multi-Format Serialization
' =============================================================================
' Tests: ParseResult, Parse, Format, FormatPretty, IsValid, Detect,
'        AutoParseResult, AutoParse, Convert, FormatName, MimeType, FormatFromName
' =============================================================================

PRINT "=== API Audit: Zanna.Data.Serialize ==="

' Format constants: 0=JSON, 1=XML, 2=YAML, 3=TOML, 4=CSV

' --- FormatName ---
PRINT "--- FormatName ---"
PRINT "FormatName(0) JSON: "; Zanna.Data.Serialize.FormatName(0)
PRINT "FormatName(1) XML: "; Zanna.Data.Serialize.FormatName(1)
PRINT "FormatName(2) YAML: "; Zanna.Data.Serialize.FormatName(2)
PRINT "FormatName(3) TOML: "; Zanna.Data.Serialize.FormatName(3)
PRINT "FormatName(4) CSV: "; Zanna.Data.Serialize.FormatName(4)

' --- MimeType ---
PRINT "--- MimeType ---"
PRINT "MimeType(0) JSON: "; Zanna.Data.Serialize.MimeType(0)
PRINT "MimeType(1) XML: "; Zanna.Data.Serialize.MimeType(1)
PRINT "MimeType(2) YAML: "; Zanna.Data.Serialize.MimeType(2)
PRINT "MimeType(3) TOML: "; Zanna.Data.Serialize.MimeType(3)
PRINT "MimeType(4) CSV: "; Zanna.Data.Serialize.MimeType(4)

' --- FormatFromName ---
PRINT "--- FormatFromName ---"
PRINT "FormatFromName('json'): "; Zanna.Data.Serialize.FormatFromName("json")
PRINT "FormatFromName('yaml'): "; Zanna.Data.Serialize.FormatFromName("yaml")
PRINT "FormatFromName('toml'): "; Zanna.Data.Serialize.FormatFromName("toml")
PRINT "FormatFromName('xml'): "; Zanna.Data.Serialize.FormatFromName("xml")
PRINT "FormatFromName('csv'): "; Zanna.Data.Serialize.FormatFromName("csv")

' --- IsValid ---
PRINT "--- IsValid ---"
DIM jsonStr AS STRING
jsonStr = "{""name"":""Alice"",""age"":30}"
PRINT "IsValid(JSON, 0): "; Zanna.Data.Serialize.IsValid(jsonStr, 0)
PRINT "IsValid('not json', 0): "; Zanna.Data.Serialize.IsValid("not json {{", 0)

' --- Detect ---
PRINT "--- Detect ---"
PRINT "Detect(JSON): "; Zanna.Data.Serialize.Detect(jsonStr)
PRINT "Detect(YAML): "; Zanna.Data.Serialize.Detect("name: Alice" & Chr(10) & "age: 30")
PRINT "Detect(TOML): "; Zanna.Data.Serialize.Detect("[person]" & Chr(10) & "name = ""Alice""")
PRINT "Detect(CSV): "; Zanna.Data.Serialize.Detect("name,age" & Chr(10) & "Alice,30")
PRINT "Detect(plain text): "; Zanna.Data.Serialize.Detect("plain text")

' --- ParseResult ---
PRINT "--- ParseResult ---"
DIM doc AS OBJECT
DIM parsed AS OBJECT
parsed = Zanna.Data.Serialize.ParseResult(jsonStr, 0)
PRINT "ParseResult IsOk: "; parsed.IsOk
doc = parsed.Unwrap()
PRINT "ParseResult(JSON) done"

DIM badParse AS OBJECT
badParse = Zanna.Data.Serialize.ParseResult("{", 0)
PRINT "Bad ParseResult IsErr: "; badParse.IsErr
PRINT "Bad ParseResult Err: "; badParse.UnwrapErrStr()

' --- Parse ---
PRINT "--- Parse ---"
DIM legacyDoc AS OBJECT
legacyDoc = Zanna.Data.Serialize.Parse(jsonStr, 0)
PRINT "Legacy Parse done"
DIM legacyBad AS OBJECT
legacyBad = Zanna.Data.Serialize.Parse("{", 0)
PRINT "Legacy Parse failure handled"

' --- Format ---
PRINT "--- Format ---"
PRINT "Format(doc, 0): "; Zanna.Data.Serialize.Format(doc, 0)

' --- FormatPretty ---
PRINT "--- FormatPretty ---"
PRINT "FormatPretty(doc, 0, 2):"
PRINT Zanna.Data.Serialize.FormatPretty(doc, 0, 2)

' --- AutoParseResult ---
PRINT "--- AutoParseResult ---"
DIM auto AS OBJECT
DIM autoResult AS OBJECT
autoResult = Zanna.Data.Serialize.AutoParseResult(jsonStr)
PRINT "AutoParseResult IsOk: "; autoResult.IsOk
auto = autoResult.Unwrap()
PRINT "AutoParseResult done"
PRINT "Format: "; Zanna.Data.Serialize.Format(auto, 0)

DIM autoBad AS OBJECT
autoBad = Zanna.Data.Serialize.AutoParseResult("plain text")
PRINT "AutoParseResult bad IsErr: "; autoBad.IsErr
PRINT "AutoParseResult bad Err: "; autoBad.UnwrapErrStr()

' --- AutoParse compatibility ---
PRINT "--- AutoParse compatibility ---"
DIM autoLegacy AS OBJECT
autoLegacy = Zanna.Data.Serialize.AutoParse(jsonStr)
PRINT "AutoParse compatibility done"

' --- Convert ---
PRINT "--- Convert ---"
PRINT "Convert JSON to YAML:"
PRINT Zanna.Data.Serialize.Convert(jsonStr, 0, 2)
PRINT "Convert JSON to XML:"
PRINT Zanna.Data.Serialize.Convert(jsonStr, 0, 1)
PRINT "Convert JSON to TOML:"
PRINT Zanna.Data.Serialize.Convert(jsonStr, 0, 3)
PRINT "Convert JSON to CSV:"
PRINT Zanna.Data.Serialize.Convert(jsonStr, 0, 4)

PRINT "=== Serialize Demo Complete ==="
END
