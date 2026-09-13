' =============================================================================
' API Audit: Zanna.Data.Xml - XML Processing
' =============================================================================
' Tests: ParseResult, Parse, IsValid, Element, Text, Comment, Cdata,
'        NodeType, Tag, Content, TextContent, Attr, HasAttr, SetAttr, RemoveAttr, AttrNames, Children,
'        ChildCount, ChildAt, Append, Remove, Find, FindAll, Format,
'        FormatPretty, Escape, Unescape
' =============================================================================

PRINT "=== API Audit: Zanna.Data.Xml ==="

' --- IsValid ---
PRINT "--- IsValid ---"
PRINT "IsValid('<root/>): "; Zanna.Data.Xml.IsValid("<root><item/></root>")
PRINT "IsValid('not xml'): "; Zanna.Data.Xml.IsValid("not xml")

' --- ParseResult ---
PRINT "--- ParseResult ---"
DIM doc AS OBJECT
DIM parsed AS OBJECT
parsed = Zanna.Data.Xml.ParseResult("<root><item id=""1"">Hello</item><item id=""2"">World</item></root>")
PRINT "ParseResult IsOk: "; parsed.IsOk
doc = parsed.Unwrap()
PRINT "ParseResult done"

DIM badParse AS OBJECT
badParse = Zanna.Data.Xml.ParseResult("<root")
PRINT "Bad ParseResult IsErr: "; badParse.IsErr
PRINT "Bad ParseResult Err: "; badParse.UnwrapErrStr()

' --- Parse ---
PRINT "--- Parse ---"
DIM legacyDoc AS OBJECT
legacyDoc = Zanna.Data.Xml.Parse("<root/>")
PRINT "Legacy Parse done"
DIM legacyBad AS OBJECT
legacyBad = Zanna.Data.Xml.Parse("<root")
PRINT "Legacy Parse failure handled"

' --- NodeType ---
PRINT "--- NodeType ---"
PRINT "NodeType: "; Zanna.Data.Xml.NodeType(doc)

' --- Tag ---
PRINT "--- Tag ---"
PRINT "Tag: "; Zanna.Data.Xml.Tag(doc)

' --- TextContent ---
PRINT "--- TextContent ---"
PRINT "TextContent: "; Zanna.Data.Xml.TextContent(doc)

' --- ChildCount ---
PRINT "--- ChildCount ---"
PRINT "ChildCount: "; Zanna.Data.Xml.ChildCount(doc)

' --- ChildAt ---
PRINT "--- ChildAt ---"
DIM child0 AS OBJECT
child0 = Zanna.Data.Xml.ChildAt(doc, 0)
PRINT "ChildAt(0) Tag: "; Zanna.Data.Xml.Tag(child0)
PRINT "ChildAt(0) Text: "; Zanna.Data.Xml.TextContent(child0)

' --- Attr ---
PRINT "--- Attr ---"
PRINT "Attr(child0, 'id'): "; Zanna.Data.Xml.Attr(child0, "id")

' --- HasAttr ---
PRINT "--- HasAttr ---"
PRINT "HasAttr('id'): "; Zanna.Data.Xml.HasAttr(child0, "id")
PRINT "HasAttr('class'): "; Zanna.Data.Xml.HasAttr(child0, "class")

' --- SetAttr ---
PRINT "--- SetAttr ---"
Zanna.Data.Xml.SetAttr(child0, "class", "primary")
PRINT "SetAttr done, Attr('class'): "; Zanna.Data.Xml.Attr(child0, "class")

' --- RemoveAttr ---
PRINT "--- RemoveAttr ---"
PRINT "RemoveAttr('class'): "; Zanna.Data.Xml.RemoveAttr(child0, "class")
PRINT "HasAttr after: "; Zanna.Data.Xml.HasAttr(child0, "class")

' --- AttrNames ---
PRINT "--- AttrNames ---"
DIM names AS OBJECT
names = Zanna.Data.Xml.AttrNames(child0)
PRINT "AttrNames returned"

' --- Element ---
PRINT "--- Element ---"
DIM elem AS OBJECT
elem = Zanna.Data.Xml.Element("newTag")
PRINT "Element('newTag') Tag: "; Zanna.Data.Xml.Tag(elem)

' --- Text ---
PRINT "--- Text ---"
DIM textNode AS OBJECT
textNode = Zanna.Data.Xml.Text("some text")
PRINT "Text node Content: "; Zanna.Data.Xml.Content(textNode)

' --- Comment ---
PRINT "--- Comment ---"
DIM commentNode AS OBJECT
commentNode = Zanna.Data.Xml.Comment("a comment")
PRINT "Comment Content: "; Zanna.Data.Xml.Content(commentNode)

' --- Cdata ---
PRINT "--- Cdata ---"
DIM cdataNode AS OBJECT
cdataNode = Zanna.Data.Xml.Cdata("raw <data>")
PRINT "Cdata Content: "; Zanna.Data.Xml.Content(cdataNode)

' --- Append ---
PRINT "--- Append ---"
DIM newChild AS OBJECT
newChild = Zanna.Data.Xml.Element("item")
Zanna.Data.Xml.SetAttr(newChild, "id", "3")
Zanna.Data.Xml.Append(doc, newChild)
PRINT "ChildCount after append: "; Zanna.Data.Xml.ChildCount(doc)

' --- Remove ---
PRINT "--- Remove ---"
PRINT "Remove: "; Zanna.Data.Xml.Remove(doc, newChild)
PRINT "ChildCount after remove: "; Zanna.Data.Xml.ChildCount(doc)

' --- Find ---
PRINT "--- Find ---"
DIM found AS OBJECT
found = Zanna.Data.Xml.Find(doc, "item")
PRINT "Find IsSome: "; found.IsSome
PRINT "Find('item') Tag: "; Zanna.Data.Xml.Tag(found.Unwrap())
PRINT "Find missing: "; Zanna.Data.Xml.Find(doc, "missing").IsNone

' --- FindAll ---
PRINT "--- FindAll ---"
DIM allItems AS OBJECT
allItems = Zanna.Data.Xml.FindAll(doc, "item")
PRINT "FindAll returned"

' --- Format ---
PRINT "--- Format ---"
PRINT "Format: "; Zanna.Data.Xml.Format(doc)

' --- FormatPretty ---
PRINT "--- FormatPretty ---"
PRINT "FormatPretty:"
PRINT Zanna.Data.Xml.FormatPretty(doc, 2)

' --- Escape ---
PRINT "--- Escape ---"
PRINT "Escape: "; Zanna.Data.Xml.Escape("<tag>&")

' --- Unescape ---
PRINT "--- Unescape ---"
PRINT "Unescape: "; Zanna.Data.Xml.Unescape("&lt;tag&gt;")

PRINT "=== Xml Demo Complete ==="
END
