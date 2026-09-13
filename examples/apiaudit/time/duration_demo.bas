' =============================================================================
' API Audit: Zanna.Time.Duration - Duration Handling
' =============================================================================
' Tests: FromMillis, FromSeconds, FromMinutes, FromHours, FromDays, Create, Zero,
'        TotalMillis, TotalSeconds, TotalMinutes, TotalHours, TotalDays,
'        TotalSecondsF, Days, Hours, Minutes, Seconds, Millis,
'        Add, Sub, Mul, Div, Abs, Neg, Cmp, ToString, ToISO
' =============================================================================

PRINT "=== API Audit: Zanna.Time.Duration ==="

' --- FromMillis ---
PRINT "--- FromMillis ---"
DIM ms AS INTEGER
ms = Zanna.Time.Duration.FromMillis(5000)
PRINT "FromMillis(5000): "; ms

' --- FromSeconds ---
PRINT "--- FromSeconds ---"
DIM sec AS INTEGER
sec = Zanna.Time.Duration.FromSeconds(90)
PRINT "FromSeconds(90): "; sec

' --- FromMinutes ---
PRINT "--- FromMinutes ---"
DIM mn AS INTEGER
mn = Zanna.Time.Duration.FromMinutes(5)
PRINT "FromMinutes(5): "; mn

' --- FromHours ---
PRINT "--- FromHours ---"
DIM hr AS INTEGER
hr = Zanna.Time.Duration.FromHours(2)
PRINT "FromHours(2): "; hr

' --- FromDays ---
PRINT "--- FromDays ---"
DIM dy AS INTEGER
dy = Zanna.Time.Duration.FromDays(1)
PRINT "FromDays(1): "; dy

' --- Create ---
PRINT "--- Create ---"
DIM cx AS INTEGER
cx = Zanna.Time.Duration.FromParts(1, 2, 30, 15, 500)
PRINT "Create(1,2,30,15,500): "; cx

' --- Zero ---
PRINT "--- Zero ---"
PRINT "Zero: "; Zanna.Time.Duration.Zero()

' --- Totals ---
PRINT "--- Totals ---"
PRINT "TotalMillis(90s): "; Zanna.Time.Duration.TotalMillis(sec)
PRINT "TotalSeconds(5m): "; Zanna.Time.Duration.TotalSeconds(mn)
PRINT "TotalMinutes(2h): "; Zanna.Time.Duration.TotalMinutes(hr)
PRINT "TotalHours(1d): "; Zanna.Time.Duration.TotalHours(dy)
PRINT "TotalDays(1d): "; Zanna.Time.Duration.TotalDays(dy)
PRINT "TotalSecondsF(5500ms): "; Zanna.Time.Duration.TotalSecondsF(Zanna.Time.Duration.FromMillis(5500))

' --- Components ---
PRINT "--- Components ---"
PRINT "Days: "; Zanna.Time.Duration.get_Days(cx)
PRINT "Hours: "; Zanna.Time.Duration.get_Hours(cx)
PRINT "Minutes: "; Zanna.Time.Duration.get_Minutes(cx)
PRINT "Seconds: "; Zanna.Time.Duration.get_Seconds(cx)
PRINT "Millis: "; Zanna.Time.Duration.get_Millis(cx)

' --- Arithmetic ---
PRINT "--- Arithmetic ---"
PRINT "Add(90s,5m) TotalSec: "; Zanna.Time.Duration.TotalSeconds(Zanna.Time.Duration.Add(sec, mn))
PRINT "Sub(5m,90s) TotalSec: "; Zanna.Time.Duration.TotalSeconds(Zanna.Time.Duration.Sub(mn, sec))
PRINT "Mul(90s,2) TotalSec: "; Zanna.Time.Duration.TotalSeconds(Zanna.Time.Duration.Mul(sec, 2))
PRINT "Div(90s,2) TotalSec: "; Zanna.Time.Duration.TotalSeconds(Zanna.Time.Duration.Div(sec, 2))

' --- Abs / Neg ---
PRINT "--- Abs / Neg ---"
DIM negD AS INTEGER
negD = Zanna.Time.Duration.Negate(sec)
PRINT "Neg(90s) TotalMillis: "; Zanna.Time.Duration.TotalMillis(negD)
PRINT "Abs(neg) TotalSec: "; Zanna.Time.Duration.TotalSeconds(Zanna.Time.Duration.Abs(negD))

' --- Cmp ---
PRINT "--- Cmp ---"
PRINT "Cmp(90s,5m): "; Zanna.Time.Duration.Compare(sec, mn)
PRINT "Cmp(5m,90s): "; Zanna.Time.Duration.Compare(mn, sec)
PRINT "Cmp(90s,90s): "; Zanna.Time.Duration.Compare(sec, sec)

' --- ToString / ToISO ---
PRINT "--- ToString / ToISO ---"
PRINT "ToString: "; Zanna.Time.Duration.ToString(cx)
PRINT "ToISO: "; Zanna.Time.Duration.ToIso8601(cx)

PRINT "=== Duration Demo Complete ==="
END
