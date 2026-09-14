' scanner_demo.bas - Comprehensive API audit for Zanna.Text.Scanner
' Tests: New, Pos, IsEnd, Remaining, Len, Reset, Peek, PeekAt, PeekStr,
'        Read, ReadStr, ReadUntil, ReadUntilAny, Match, MatchStr,
'        Accept, AcceptStr, AcceptAny, Skip, SkipWhitespace,
'        ReadIdent, ReadIntToken, ReadNumberToken, ReadQuoted, ReadLine

PRINT "=== Scanner API Audit ==="

' --- New ---
PRINT "--- New ---"
DIM sc AS Zanna.Text.Scanner
sc = Zanna.Text.Scanner.New("Hello, World! 42 3.14 ""quoted""")
PRINT sc.Position         ' 0
PRINT sc.IsEnd       ' 0
PRINT sc.Length         ' string length
PRINT sc.Remaining   ' same as Len at start

' --- Peek (returns char code at current position) ---
PRINT "--- Peek ---"
PRINT sc.Peek()      ' 72 ('H')

' --- PeekAt (char at offset) ---
PRINT "--- PeekAt ---"
PRINT sc.PeekAt(1)   ' 101 ('e')
PRINT sc.PeekAt(4)   ' 111 ('o')

' --- PeekStr (peek N chars as string) ---
PRINT "--- PeekStr ---"
PRINT sc.PeekStr(5)  ' Hello

' --- Read (consume one char, return char code) ---
PRINT "--- Read ---"
PRINT sc.Read()      ' 72 ('H')
PRINT sc.Position         ' 1
PRINT sc.Read()      ' 101 ('e')
PRINT sc.Position         ' 2

' --- ReadStr (consume N chars as string) ---
PRINT "--- ReadStr ---"
PRINT sc.ReadStr(3)  ' llo
PRINT sc.Position         ' 5

' --- Match (check if current char matches, no consume) ---
PRINT "--- Match ---"
PRINT sc.Match(44)   ' 1 (44 = ',')
PRINT sc.Position         ' 5

' --- Accept (consume if matches) ---
PRINT "--- Accept ---"
PRINT sc.Accept(44)  ' 1 (consumed ',')
PRINT sc.Position         ' 6
PRINT sc.Accept(44)  ' 0 (next is ' ', not ',')

' --- Skip ---
PRINT "--- Skip ---"
sc.Skip(1)           ' skip the space
PRINT sc.Position         ' 7

' --- MatchStr ---
PRINT "--- MatchStr ---"
PRINT sc.MatchStr("World")  ' 1
PRINT sc.Position                 ' 7

' --- AcceptStr ---
PRINT "--- AcceptStr ---"
PRINT sc.AcceptStr("World") ' 1 (consumed)
PRINT sc.Position                 ' 12

' --- AcceptAny (accept any char from set) ---
PRINT "--- AcceptAny ---"
PRINT sc.AcceptAny("!?")  ' 1 (consumed '!')
PRINT sc.Position               ' 13

' --- SkipWhitespace ---
PRINT "--- SkipWhitespace ---"
PRINT sc.SkipWhitespace()  ' number of whitespace chars skipped

' --- ReadIntToken ---
PRINT "--- ReadIntToken ---"
PRINT sc.ReadIntToken()   ' 42

' --- SkipWhitespace again ---
sc.SkipWhitespace()

' --- ReadNumberToken (reads float) ---
PRINT "--- ReadNumberToken ---"
PRINT sc.ReadNumberToken()  ' 3.14

' --- SkipWhitespace ---
sc.SkipWhitespace()

' --- ReadQuoted ---
PRINT "--- ReadQuoted ---"
PRINT sc.ReadQuoted(34)  ' quoted (34 = '"')

' --- IsEnd ---
PRINT "--- IsEnd ---"
PRINT sc.IsEnd       ' 1

' --- Reset ---
PRINT "--- Reset ---"
sc.Reset()
PRINT sc.Position         ' 0
PRINT sc.IsEnd       ' 0

' --- Pos (set) ---
PRINT "--- Pos set ---"
sc.Position = 7
PRINT sc.Position         ' 7

' --- ReadUntil (read until char) ---
PRINT "--- ReadUntil ---"
sc.Reset()
PRINT sc.ReadUntil(44)   ' Hello (read until ',')

' --- ReadUntilAny ---
PRINT "--- ReadUntilAny ---"
sc.Reset()
PRINT sc.ReadUntilAny(",!")  ' Hello

' --- ReadIdent ---
PRINT "--- ReadIdent ---"
DIM sc2 AS Zanna.Text.Scanner
sc2 = Zanna.Text.Scanner.New("myVar123 + other")
PRINT sc2.ReadIdent()  ' myVar123

' --- ReadLine ---
PRINT "--- ReadLine ---"
DIM sc3 AS Zanna.Text.Scanner
sc3 = Zanna.Text.Scanner.New("line one" + CHR$(10) + "line two" + CHR$(10) + "line three")
PRINT sc3.ReadLine()   ' line one
PRINT sc3.ReadLine()   ' line two

' --- Remaining ---
PRINT "--- Remaining ---"
PRINT sc3.Remaining    ' remaining chars

PRINT "=== Scanner audit complete ==="
END
