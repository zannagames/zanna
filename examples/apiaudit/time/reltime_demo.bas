' =============================================================================
' API Audit: Zanna.Time.RelativeTime - Human-Readable Time Formatting
' =============================================================================
' Tests: Format, FormatFrom, FormatDuration, FormatShort
' =============================================================================

PRINT "=== API Audit: Zanna.Time.RelativeTime ==="

DIM now AS INTEGER
now = Zanna.Time.DateTime.Now()

' --- Format ---
PRINT "--- Format ---"
PRINT "Format(now): "; Zanna.Time.RelativeTime.Format(now)

DIM hourAgo AS INTEGER
hourAgo = Zanna.Time.DateTime.AddSeconds(now, -3600)
PRINT "Format(1h ago): "; Zanna.Time.RelativeTime.Format(hourAgo)

DIM dayAgo AS INTEGER
dayAgo = Zanna.Time.DateTime.AddDays(now, -1)
PRINT "Format(1d ago): "; Zanna.Time.RelativeTime.Format(dayAgo)

DIM weekAgo AS INTEGER
weekAgo = Zanna.Time.DateTime.AddDays(now, -7)
PRINT "Format(7d ago): "; Zanna.Time.RelativeTime.Format(weekAgo)

' --- FormatFrom ---
PRINT "--- FormatFrom ---"
DIM ts1 AS INTEGER
DIM ts2 AS INTEGER
ts1 = Zanna.Time.DateTime.FromParts(2024, 1, 1, 0, 0, 0)
ts2 = Zanna.Time.DateTime.FromParts(2024, 1, 2, 0, 0, 0)
PRINT "FormatFrom(Jan1, Jan2): "; Zanna.Time.RelativeTime.FormatFrom(ts1, ts2)

' --- FormatDuration ---
PRINT "--- FormatDuration ---"
PRINT "FormatDuration(5s): "; Zanna.Time.RelativeTime.FormatDuration(Zanna.Time.Duration.FromSeconds(5))
PRINT "FormatDuration(90s): "; Zanna.Time.RelativeTime.FormatDuration(Zanna.Time.Duration.FromSeconds(90))
PRINT "FormatDuration(2h): "; Zanna.Time.RelativeTime.FormatDuration(Zanna.Time.Duration.FromHours(2))

' --- FormatShort ---
PRINT "--- FormatShort ---"
PRINT "FormatShort(now): "; Zanna.Time.RelativeTime.FormatShort(now)
PRINT "FormatShort(1h ago): "; Zanna.Time.RelativeTime.FormatShort(hourAgo)
PRINT "FormatShort(1d ago): "; Zanna.Time.RelativeTime.FormatShort(dayAgo)

PRINT "=== RelativeTime Demo Complete ==="
END
