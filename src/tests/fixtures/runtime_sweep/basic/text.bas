' EXPECT_OUT: RESULT: ok
' COVER: Zanna.Text.Codec.Base64Decode
' COVER: Zanna.Text.Codec.Base64Encode
' COVER: Zanna.Text.Codec.HexDecode
' COVER: Zanna.Text.Codec.HexEncode
' COVER: Zanna.Text.Codec.UrlDecode
' COVER: Zanna.Text.Codec.UrlEncode
' COVER: Zanna.Data.Csv.Format
' COVER: Zanna.Data.Csv.FormatLine
' COVER: Zanna.Data.Csv.FormatLineWith
' COVER: Zanna.Data.Csv.FormatWith
' COVER: Zanna.Data.Csv.Parse
' COVER: Zanna.Data.Csv.ParseLine
' COVER: Zanna.Data.Csv.ParseLineWith
' COVER: Zanna.Data.Csv.ParseWith
' COVER: Zanna.Text.Uuid.Empty
' COVER: Zanna.Text.Uuid.FromBytes
' COVER: Zanna.Text.Uuid.IsValid
' COVER: Zanna.Text.Uuid.Generate
' COVER: Zanna.Text.Uuid.ToBytes
' COVER: Zanna.Text.Pattern.Escape
' COVER: Zanna.Text.Pattern.Find
' COVER: Zanna.Text.Pattern.FindAll
' COVER: Zanna.Text.Pattern.FindFrom
' COVER: Zanna.Text.Pattern.FindPos
' COVER: Zanna.Text.Pattern.IsMatch
' COVER: Zanna.Text.Pattern.Replace
' COVER: Zanna.Text.Pattern.ReplaceFirst
' COVER: Zanna.Text.Pattern.Split
' COVER: Zanna.Text.StringBuilder.New
' COVER: Zanna.Text.StringBuilder.Capacity
' COVER: Zanna.Text.StringBuilder.Length
' COVER: Zanna.Text.StringBuilder.Append
' COVER: Zanna.Text.StringBuilder.AppendLine
' COVER: Zanna.Text.StringBuilder.Clear
' COVER: Zanna.Text.StringBuilder.ToString
' COVER: Zanna.Text.Template.Escape
' COVER: Zanna.Text.Template.Has
' COVER: Zanna.Text.Template.Keys
' COVER: Zanna.Text.Template.Render
' COVER: Zanna.Text.Template.RenderSeq
' COVER: Zanna.Text.Template.RenderWith

DIM encoded AS STRING
encoded = Zanna.Text.Codec.Base64Encode("Hello")
Zanna.Core.Diagnostics.AssertEqStr(encoded, "SGVsbG8=", "codec.b64enc")
Zanna.Core.Diagnostics.AssertEqStr(Zanna.Text.Codec.Base64Decode(encoded), "Hello", "codec.b64dec")
Zanna.Core.Diagnostics.AssertEqStr(Zanna.Text.Codec.HexEncode("ABC"), "414243", "codec.hexenc")
Zanna.Core.Diagnostics.AssertEqStr(Zanna.Text.Codec.HexDecode("414243"), "ABC", "codec.hexdec")
Zanna.Core.Diagnostics.AssertEqStr(Zanna.Text.Codec.UrlEncode("hello world"), "hello%20world", "codec.urlenc")
Zanna.Core.Diagnostics.AssertEqStr(Zanna.Text.Codec.UrlDecode("hello%20world"), "hello world", "codec.urldec")

DIM fields AS Zanna.Collections.Seq
fields = Zanna.Data.Csv.ParseLine("a,b,c")
Zanna.Core.Diagnostics.AssertEq(fields.Count, 3, "csv.parseline.len")
Zanna.Core.Diagnostics.AssertEqStr(Zanna.Core.Box.ToStr(fields.Get(1)), "b", "csv.parseline.get")

DIM row AS Zanna.Collections.Seq
row = Zanna.Data.Csv.ParseLine("""He said """"Hi""""""")
Zanna.Core.Diagnostics.AssertEqStr(Zanna.Core.Box.ToStr(row.Get(0)), "He said ""Hi""", "csv.quotes")

DIM rows AS Zanna.Collections.Seq
rows = Zanna.Data.Csv.Parse("a,b" + Zanna.String.Chr(10) + "c,d")
Zanna.Core.Diagnostics.AssertEq(rows.Count, 2, "csv.parse.len")
DIM row0 AS Zanna.Collections.Seq
row0 = rows.Get(0)
Zanna.Core.Diagnostics.AssertEqStr(Zanna.Core.Box.ToStr(row0.Get(0)), "a", "csv.parse.row0")

DIM line AS STRING
line = Zanna.Data.Csv.FormatLine(fields)
Zanna.Core.Diagnostics.Assert(line <> "", "csv.formatline")
DIM line2 AS STRING
line2 = Zanna.Data.Csv.FormatLineWith(fields, "|")
Zanna.Core.Diagnostics.Assert(line2 <> "", "csv.formatlinewith")

DIM rowsOut AS STRING
rowsOut = Zanna.Data.Csv.Format(rows)
Zanna.Core.Diagnostics.Assert(rowsOut <> "", "csv.format")
DIM rowsOut2 AS STRING
rowsOut2 = Zanna.Data.Csv.FormatWith(rows, "|")
Zanna.Core.Diagnostics.Assert(rowsOut2 <> "", "csv.formatwith")

DIM fields2 AS Zanna.Collections.Seq
fields2 = Zanna.Data.Csv.ParseLineWith("a|b|c", "|")
Zanna.Core.Diagnostics.AssertEq(fields2.Count, 3, "csv.parselinewith")

DIM rows2 AS Zanna.Collections.Seq
rows2 = Zanna.Data.Csv.ParseWith("a|b" + Zanna.String.Chr(10) + "c|d", "|")
Zanna.Core.Diagnostics.AssertEq(rows2.Count, 2, "csv.parsewith")

DIM id AS STRING
id = Zanna.Text.Uuid.Generate()
Zanna.Core.Diagnostics.Assert(Zanna.Text.Uuid.IsValid(id), "guid.valid")
Zanna.Core.Diagnostics.Assert(Zanna.Text.Uuid.IsValid("not-a-guid") = FALSE, "guid.invalid")
Zanna.Core.Diagnostics.AssertEqStr(Zanna.Text.Uuid.Empty, "00000000-0000-0000-0000-000000000000", "guid.empty")
DIM gidBytes AS Zanna.Collections.Bytes
gidBytes = Zanna.Text.Uuid.ToBytes(id)
DIM id2 AS STRING
id2 = Zanna.Text.Uuid.FromBytes(gidBytes)
Zanna.Core.Diagnostics.Assert(Zanna.Text.Uuid.IsValid(id2), "guid.frombytes")

DIM text AS STRING
text = "abc123def456"
Zanna.Core.Diagnostics.Assert(Zanna.Text.Pattern.IsMatch(text, "\d+"), "pat.ismatch")
Zanna.Core.Diagnostics.AssertEqStr(Zanna.Option.UnwrapStr(Zanna.Text.Pattern.Find(text, "\d+")), "123", "pat.find")
Zanna.Core.Diagnostics.AssertEqStr(Zanna.Option.UnwrapStr(Zanna.Text.Pattern.FindFrom(text, "\d+", 3)), "123", "pat.findfrom")
Zanna.Core.Diagnostics.AssertEq(Zanna.Option.UnwrapOrI64(Zanna.Text.Pattern.FindPos("Hello World", "World"), -1), 6, "pat.findpos")
DIM matches AS Zanna.Collections.Seq
matches = Zanna.Text.Pattern.FindAll(text, "\d+")
Zanna.Core.Diagnostics.AssertEq(matches.Count, 2, "pat.findall")
Zanna.Core.Diagnostics.AssertEqStr(Zanna.Text.Pattern.Replace(text, "\d+", "X"), "abcXdefX", "pat.replace")
Zanna.Core.Diagnostics.AssertEqStr(Zanna.Text.Pattern.ReplaceFirst(text, "\d+", "X"), "abcXdef456", "pat.replacefirst")
DIM parts AS Zanna.Collections.Seq
parts = Zanna.Text.Pattern.Split("hello   world  test", "\s+")
Zanna.Core.Diagnostics.AssertEq(parts.Count, 3, "pat.split")
Zanna.Core.Diagnostics.AssertEqStr(Zanna.Text.Pattern.Escape("file.txt"), "file\.txt", "pat.escape")

DIM values AS Zanna.Collections.Map
values = Zanna.Collections.Map.New()
values.Set("name", Zanna.Core.Box.Str("Alice"))
values.Set("count", Zanna.Core.Box.Str("5"))
DIM templ AS STRING
templ = "Hello {{name}}, you have {{count}} messages."
DIM rendered AS STRING
rendered = Zanna.Text.Template.Render(templ, values)
Zanna.Core.Diagnostics.AssertEqStr(rendered, "Hello Alice, you have 5 messages.", "tmpl.render")
DIM seqVals AS Zanna.Collections.Seq
seqVals = Zanna.Collections.Seq.New()
seqVals.Push(Zanna.Core.Box.Str("Alice"))
seqVals.Push(Zanna.Core.Box.Str("Bob"))
seqVals.Push(Zanna.Core.Box.Str("Charlie"))
DIM renderedSeq AS STRING
renderedSeq = Zanna.Text.Template.RenderSeq("{{0}} and {{1}} meet {{2}}", seqVals)
Zanna.Core.Diagnostics.AssertEqStr(renderedSeq, "Alice and Bob meet Charlie", "tmpl.renderseq")
DIM renderedWith AS STRING
renderedWith = Zanna.Text.Template.RenderWith("Hello $name$!", values, "$", "$")
Zanna.Core.Diagnostics.AssertEqStr(renderedWith, "Hello Alice!", "tmpl.renderwith")
Zanna.Core.Diagnostics.Assert(Zanna.Text.Template.Has(templ, "name"), "tmpl.has")
DIM keys AS Zanna.Collections.StringSet
keys = Zanna.Text.Template.Keys(templ)
Zanna.Core.Diagnostics.Assert(keys.Has("name"), "tmpl.keys")
DIM escaped AS STRING
escaped = Zanna.Text.Template.Escape("Use {{name}} for placeholders")
DIM renderedEsc AS STRING
renderedEsc = Zanna.Text.Template.Render(escaped, values)
Zanna.Core.Diagnostics.AssertEqStr(renderedEsc, "Use {{name}} for placeholders", "tmpl.escape")

DIM sb AS Zanna.Text.StringBuilder
sb = NEW Zanna.Text.StringBuilder()
Zanna.Core.Diagnostics.AssertEq(sb.Length, 0, "sb.len0")
Zanna.Core.Diagnostics.Assert(sb.Capacity >= sb.Length, "sb.capacity")
sb.Append("a")
sb.AppendLine("b")
DIM sbText AS STRING
sbText = sb.ToString()
Zanna.Core.Diagnostics.Assert(sbText <> "", "sb.tostring")
sb.Clear()
Zanna.Core.Diagnostics.AssertEq(sb.Length, 0, "sb.clear")

PRINT "RESULT: ok"
END
