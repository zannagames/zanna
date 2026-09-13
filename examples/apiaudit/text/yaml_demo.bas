' =============================================================================
' API Audit: Zanna.Data.Yaml - YAML Processing
' =============================================================================
' Tests: ParseResult, Parse, IsValid, Format, FormatIndent, TypeOf
' =============================================================================

PRINT "=== API Audit: Zanna.Data.Yaml ==="

' --- IsValid ---
PRINT "--- IsValid ---"
PRINT "IsValid('name: test'): "; Zanna.Data.Yaml.IsValid("name: test")
PRINT "IsValid('key: value'): "; Zanna.Data.Yaml.IsValid("key: value")

' --- ParseResult ---
PRINT "--- ParseResult ---"
DIM yamlStr AS STRING
yamlStr = "name: Alice" + CHR$(10) + "age: 30" + CHR$(10) + "active: true" + CHR$(10)
DIM doc AS OBJECT
DIM parsed AS OBJECT
parsed = Zanna.Data.Yaml.ParseResult(yamlStr)
PRINT "ParseResult IsOk: "; parsed.IsOk
doc = parsed.Unwrap()
PRINT "ParseResult done"

DIM nullDoc AS OBJECT
nullDoc = Zanna.Data.Yaml.ParseResult("null")
PRINT "Null ParseResult IsOk: "; nullDoc.IsOk

DIM badYaml AS OBJECT
badYaml = Zanna.Data.Yaml.ParseResult("name: [unterminated" + CHR$(10))
PRINT "Bad ParseResult IsErr: "; badYaml.IsErr
PRINT "Bad ParseResult Err: "; badYaml.UnwrapErrStr()

' --- Parse ---
PRINT "--- Parse ---"
DIM legacyDoc AS OBJECT
legacyDoc = Zanna.Data.Yaml.Parse("name: Bob" + CHR$(10))
PRINT "Legacy TypeOf: "; Zanna.Data.Yaml.TypeOf(legacyDoc)
DIM legacyBad AS OBJECT
legacyBad = Zanna.Data.Yaml.Parse("name: [unterminated" + CHR$(10))
PRINT "Legacy Parse failure handled"

' --- TypeOf ---
PRINT "--- TypeOf ---"
PRINT "TypeOf(doc): "; Zanna.Data.Yaml.TypeOf(doc)

' --- Format ---
PRINT "--- Format ---"
PRINT "Format:"
PRINT Zanna.Data.Yaml.Format(doc)

' --- FormatIndent ---
PRINT "--- FormatIndent ---"
PRINT "FormatIndent(4):"
PRINT Zanna.Data.Yaml.FormatIndent(doc, 4)

' Parse a scalar
PRINT "--- Scalar ---"
DIM scalar AS OBJECT
DIM scalarResult AS OBJECT
scalarResult = Zanna.Data.Yaml.ParseResult("42")
scalar = scalarResult.Unwrap()
PRINT "TypeOf(scalar): "; Zanna.Data.Yaml.TypeOf(scalar)

' Parse a list
PRINT "--- List ---"
DIM lst AS OBJECT
DIM listResult AS OBJECT
listResult = Zanna.Data.Yaml.ParseResult("- one" + CHR$(10) + "- two" + CHR$(10) + "- three" + CHR$(10))
lst = listResult.Unwrap()
PRINT "TypeOf(list): "; Zanna.Data.Yaml.TypeOf(lst)
PRINT "Format(list):"
PRINT Zanna.Data.Yaml.Format(lst)

PRINT "=== Yaml Demo Complete ==="
END
