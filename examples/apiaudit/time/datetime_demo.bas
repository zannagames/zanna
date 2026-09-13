' =============================================================================
' API Audit: Zanna.Time.DateTime - Date/Time Operations
' =============================================================================
' Tests: Now, NowMs, Create, Year, Month, Day, Hour, Minute, Second,
'        DayOfWeek, AddDays, AddSeconds, Diff, Format, ToISO
' =============================================================================

PRINT "=== API Audit: Zanna.Time.DateTime ==="

' --- Now ---
PRINT "--- Now ---"
DIM now AS INTEGER
now = Zanna.Time.DateTime.Now()
PRINT "Now: "; now

' --- NowMs ---
PRINT "--- NowMs ---"
PRINT "NowMs: "; Zanna.Time.DateTime.NowMs()

' --- Date components ---
PRINT "--- Date components ---"
PRINT "Year: "; Zanna.Time.DateTime.Year(now)
PRINT "Month: "; Zanna.Time.DateTime.Month(now)
PRINT "Day: "; Zanna.Time.DateTime.Day(now)
PRINT "Hour: "; Zanna.Time.DateTime.Hour(now)
PRINT "Minute: "; Zanna.Time.DateTime.Minute(now)
PRINT "Second: "; Zanna.Time.DateTime.Second(now)

' --- DayOfWeek ---
PRINT "--- DayOfWeek ---"
PRINT "DayOfWeek: "; Zanna.Time.DateTime.DayOfWeek(now)

' --- Create ---
PRINT "--- Create ---"
DIM ts AS INTEGER
ts = Zanna.Time.DateTime.FromParts(2024, 6, 15, 12, 30, 0)
PRINT "Create(2024,6,15,12,30,0): "; ts
PRINT "Year: "; Zanna.Time.DateTime.Year(ts)
PRINT "Month: "; Zanna.Time.DateTime.Month(ts)
PRINT "Day: "; Zanna.Time.DateTime.Day(ts)

' --- AddDays ---
PRINT "--- AddDays ---"
DIM tomorrow AS INTEGER
tomorrow = Zanna.Time.DateTime.AddDays(ts, 1)
PRINT "AddDays(1) Day: "; Zanna.Time.DateTime.Day(tomorrow)

' --- AddSeconds ---
PRINT "--- AddSeconds ---"
DIM plusHour AS INTEGER
plusHour = Zanna.Time.DateTime.AddSeconds(ts, 3600)
PRINT "AddSeconds(3600) Hour: "; Zanna.Time.DateTime.Hour(plusHour)

' --- Diff ---
PRINT "--- Diff ---"
PRINT "Diff(tomorrow, ts): "; Zanna.Time.DateTime.Diff(tomorrow, ts)

' --- Format ---
PRINT "--- Format ---"
PRINT "Format: "; Zanna.Time.DateTime.Format(ts, "%Y-%m-%d %H:%M:%S")

' --- ToISO ---
PRINT "--- ToISO ---"
PRINT "ToISO: "; Zanna.Time.DateTime.ToIso8601(ts)
PRINT "ToISO(0): "; Zanna.Time.DateTime.ToIso8601(0)

PRINT "=== DateTime Demo Complete ==="
END
