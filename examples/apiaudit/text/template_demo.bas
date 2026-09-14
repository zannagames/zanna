' Zanna.Text.Template API Audit - String Templating
' Tests all Template functions

PRINT "=== Zanna.Text.Template API Audit ==="

' --- Render ---
PRINT "--- Render ---"
DIM m AS OBJECT
m = Zanna.Collections.Map.New()
m.Set("name", Zanna.Core.Box.Str("Alice"))
m.Set("age", Zanna.Core.Box.Str("30"))
m.Set("city", Zanna.Core.Box.Str("Boston"))

DIM tpl AS STRING
tpl = "Hello, {{name}}! You are {{age}} years old from {{city}}."
PRINT Zanna.Text.Template.Render(tpl, m)

' Template with no variables
DIM tpl2 AS STRING
tpl2 = "No variables here."
PRINT Zanna.Text.Template.Render(tpl2, m)

' --- RenderSeq ---
PRINT "--- RenderSeq ---"
DIM items AS OBJECT
items = Zanna.Collections.Seq.New()
items.Push(Zanna.Core.Box.Str("apple"))
items.Push(Zanna.Core.Box.Str("banana"))
items.Push(Zanna.Core.Box.Str("cherry"))
DIM seqTpl AS STRING
seqTpl = "Item 0: {{0}}, Item 1: {{1}}, Item 2: {{2}}"
PRINT Zanna.Text.Template.RenderSeq(seqTpl, items)

' --- Has ---
PRINT "--- Has ---"
PRINT "Has 'name': "; Zanna.Text.Template.Has(tpl, "name")
PRINT "Has 'age': "; Zanna.Text.Template.Has(tpl, "age")
PRINT "Has 'email': "; Zanna.Text.Template.Has(tpl, "email")
PRINT "Has 'city': "; Zanna.Text.Template.Has(tpl, "city")

' --- Keys ---
PRINT "--- Keys ---"
DIM keys AS Zanna.Collections.Seq
keys = Zanna.Collections.StringSet.ToSeq(Zanna.Text.Template.Keys(tpl))
PRINT "Key count: "; keys.Count
DIM ki AS INTEGER
FOR ki = 0 TO keys.Count - 1
    PRINT "Key: "; Zanna.Core.Box.ToStr(keys.Get(ki))
NEXT ki

' --- Escape ---
PRINT "--- Escape ---"
PRINT Zanna.Text.Template.Escape("Hello {{name}}, use \{{literal}} braces")
PRINT Zanna.Text.Template.Escape("No braces here")

PRINT "=== Template Demo Complete ==="
END
