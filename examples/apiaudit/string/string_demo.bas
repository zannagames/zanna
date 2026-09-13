' Zanna.String API Audit - Comprehensive Demo
' Tests all Zanna.String functions

PRINT "=== Zanna.String API Audit ==="

' --- get_Length ---
PRINT "--- get_Length ---"
PRINT Zanna.String.get_Length("hello")
PRINT Zanna.String.get_Length("")
PRINT Zanna.String.get_Length("Hello, World!")

' --- get_IsEmpty ---
PRINT "--- get_IsEmpty ---"
PRINT Zanna.String.get_IsEmpty("")
PRINT Zanna.String.get_IsEmpty("x")

' --- Trim / TrimStart / TrimEnd ---
PRINT "--- Trim ---"
PRINT "["; Zanna.String.Trim("  hello  "); "]"
PRINT "["; Zanna.String.TrimStart("  hello  "); "]"
PRINT "["; Zanna.String.TrimEnd("  hello  "); "]"

' --- ToUpper / ToLower ---
PRINT "--- ToUpper / ToLower ---"
PRINT Zanna.String.ToUpper("hello world")
PRINT Zanna.String.ToLower("HELLO WORLD")

' --- Left / Right ---
PRINT "--- Left / Right ---"
PRINT Zanna.String.Left("Hello, World!", 5)
PRINT Zanna.String.Right("Hello, World!", 6)

' --- Mid / MidLen / Substring ---
PRINT "--- Mid / MidLen / Substring ---"
PRINT Zanna.String.Mid("Hello, World!", 8)
PRINT Zanna.String.MidLen("Hello, World!", 1, 5)
PRINT Zanna.String.Substring("Hello, World!", 7, 5)

' --- IndexOf / IndexOfFrom / LastIndexOf ---
PRINT "--- IndexOf / IndexOfFrom / LastIndexOf ---"
PRINT "IndexOf 'world': "; Zanna.String.IndexOf("hello world", "world")
PRINT "IndexOf 'xyz': "; Zanna.String.IndexOf("hello world", "xyz")
PRINT "IndexOfFrom: "; Zanna.String.IndexOfFrom("hello world hello", 6, "hello")
PRINT "LastIndexOf: "; Zanna.String.LastIndexOf("hello world hello", "hello")

' --- Has / StartsWith / EndsWith ---
PRINT "--- Has / StartsWith / EndsWith ---"
PRINT "Contains 'world': "; Zanna.String.Contains("hello world", "world")
PRINT "Contains 'xyz': "; Zanna.String.Contains("hello world", "xyz")
PRINT "StartsWith 'hello': "; Zanna.String.StartsWith("hello world", "hello")
PRINT "StartsWith 'world': "; Zanna.String.StartsWith("hello world", "world")
PRINT "EndsWith 'world': "; Zanna.String.EndsWith("hello world", "world")
PRINT "EndsWith 'hello': "; Zanna.String.EndsWith("hello world", "hello")

' --- Replace ---
PRINT "--- Replace ---"
PRINT Zanna.String.Replace("hello world", "world", "zanna")
PRINT Zanna.String.Replace("aaa", "a", "bb")

' --- Split / Join ---
PRINT "--- Split / Join ---"
DIM parts AS Zanna.Collections.Seq
parts = Zanna.String.Split("one,two,three", ",")
PRINT "Split count: "; parts.Count
DIM sp0 AS OBJECT
sp0 = parts.Get(0)
PRINT "Part 0: "; sp0
DIM sp1 AS OBJECT
sp1 = parts.Get(1)
PRINT "Part 1: "; sp1
DIM sp2 AS OBJECT
sp2 = parts.Get(2)
PRINT "Part 2: "; sp2
PRINT "Join: "; Zanna.String.Join("-", parts)

' --- Flip ---
PRINT "--- Flip ---"
PRINT Zanna.String.Reverse("abcde")
PRINT Zanna.String.Reverse("racecar")

' --- Repeat ---
PRINT "--- Repeat ---"
PRINT Zanna.String.Repeat("ab", 3)
PRINT Zanna.String.Repeat("x", 5)

' --- PadLeft / PadRight ---
PRINT "--- PadLeft / PadRight ---"
PRINT "["; Zanna.String.PadLeft("42", 6, "0"); "]"
PRINT "["; Zanna.String.PadRight("hi", 6, "."); "]"

' --- Count ---
PRINT "--- Count ---"
PRINT "Count 'a' in 'banana': "; Zanna.String.Count("banana", "a")
PRINT "Count 'na' in 'banana': "; Zanna.String.Count("banana", "na")
PRINT "Count 'z' in 'hello': "; Zanna.String.Count("hello", "z")

' --- Cmp / CmpNoCase / Equals ---
PRINT "--- Cmp / CmpNoCase / Equals ---"
PRINT "Compare apple/banana: "; Zanna.String.Compare("apple", "banana")
PRINT "Compare banana/apple: "; Zanna.String.Compare("banana", "apple")
PRINT "Compare apple/apple: "; Zanna.String.Compare("apple", "apple")
PRINT "CmpNoCase Hello/hello: "; Zanna.String.CompareIgnoreCase("Hello", "hello")
PRINT "Equals abc/abc: "; Zanna.String.Equals("abc", "abc")
PRINT "Equals abc/xyz: "; Zanna.String.Equals("abc", "xyz")

' --- Asc / Chr ---
PRINT "--- Asc / Chr ---"
PRINT "Asc('A'): "; Zanna.String.Asc("A")
PRINT "Asc('a'): "; Zanna.String.Asc("a")
PRINT "Chr(65): "; Zanna.String.Chr(65)
PRINT "Chr(97): "; Zanna.String.Chr(97)

' --- Concat ---
PRINT "--- Concat ---"
PRINT Zanna.String.Concat("Hello, ", "World!")

' --- Capitalize / Title ---
PRINT "--- Capitalize / Title ---"
PRINT Zanna.String.Capitalize("hello world")
PRINT Zanna.String.Title("hello world foo")

' --- RemovePrefix / RemoveSuffix ---
PRINT "--- RemovePrefix / RemoveSuffix ---"
PRINT Zanna.String.RemovePrefix("HelloWorld", "Hello")
PRINT Zanna.String.RemovePrefix("HelloWorld", "Bye")
PRINT Zanna.String.RemoveSuffix("HelloWorld", "World")
PRINT Zanna.String.RemoveSuffix("HelloWorld", "Bye")

' --- TrimChar ---
PRINT "--- TrimChar ---"
PRINT Zanna.String.TrimChar("***hello***", "*")
PRINT Zanna.String.TrimChar("xxhelloxx", "x")

' --- Slug ---
PRINT "--- Slug ---"
PRINT Zanna.String.Slug("Hello, World!  How Are You?")
PRINT Zanna.String.Slug("  The Quick Brown Fox  ")

' --- CamelCase / PascalCase / SnakeCase / KebabCase / ScreamingSnake ---
PRINT "--- Case Conversions ---"
PRINT "CamelCase: "; Zanna.String.CamelCase("hello world")
PRINT "PascalCase: "; Zanna.String.PascalCase("hello world")
PRINT "SnakeCase: "; Zanna.String.SnakeCase("helloWorld")
PRINT "KebabCase: "; Zanna.String.KebabCase("helloWorld")
PRINT "ScreamingSnake: "; Zanna.String.ScreamingSnakeCase("helloWorld")

' --- Levenshtein ---
PRINT "--- Levenshtein ---"
PRINT "kitten/sitting: "; Zanna.String.Levenshtein("kitten", "sitting")
PRINT "hello/hello: "; Zanna.String.Levenshtein("hello", "hello")
PRINT "empty/abc: "; Zanna.String.Levenshtein("", "abc")

' --- Jaro / JaroWinkler ---
PRINT "--- Jaro / JaroWinkler ---"
PRINT "Jaro martha/marhta: "; Zanna.String.Jaro("martha", "marhta")
PRINT "JaroWinkler martha/marhta: "; Zanna.String.JaroWinkler("martha", "marhta")
PRINT "Jaro hello/hello: "; Zanna.String.Jaro("hello", "hello")

' --- Hamming ---
PRINT "--- Hamming ---"
PRINT "karolin/kathrin: "; Zanna.String.Hamming("karolin", "kathrin")
PRINT "abc/abc: "; Zanna.String.Hamming("abc", "abc")
PRINT "abc/abcd (diff len): "; Zanna.String.Hamming("abc", "abcd")

PRINT "=== String Demo Complete ==="
END
