' test_datetime.bas — Zanna.Time.DateTime + Duration
DIM t AS INTEGER
LET t = Zanna.Time.DateTime.FromParts(2024, 6, 15, 10, 30, 0)
PRINT Zanna.Time.DateTime.Year(t)
PRINT Zanna.Time.DateTime.Month(t)
PRINT Zanna.Time.DateTime.Day(t)
PRINT Zanna.Time.DateTime.Hour(t)
PRINT Zanna.Time.DateTime.Minute(t)
PRINT Zanna.Time.DateTime.Second(t)
PRINT Zanna.Time.DateTime.DayOfWeek(t)

DIM iso AS STRING
LET iso = Zanna.Time.DateTime.ToIso8601(t)
PRINT Zanna.String.Contains(iso, "2024")

DIM t2 AS INTEGER
LET t2 = Zanna.Time.DateTime.AddDays(t, 10)
PRINT Zanna.Time.DateTime.Day(t2)

DIM t3 AS INTEGER
LET t3 = Zanna.Time.DateTime.AddSeconds(t, 3600)
PRINT Zanna.Time.DateTime.Hour(t3)

DIM now AS INTEGER
LET now = Zanna.Time.DateTime.Now()
PRINT now > 0

DIM ms AS INTEGER
LET ms = Zanna.Time.DateTime.NowMs()
PRINT ms > 0

' Duration
DIM d AS INTEGER
LET d = Zanna.Time.Duration.FromSeconds(90)
PRINT Zanna.Time.Duration.TotalSeconds(d)
PRINT Zanna.Time.Duration.TotalMillis(d)
PRINT Zanna.Time.Duration.get_Minutes(d)
PRINT Zanna.Time.Duration.get_Seconds(d)

DIM d2 AS INTEGER
LET d2 = Zanna.Time.Duration.FromParts(1, 2, 30, 0, 0)
PRINT Zanna.Time.Duration.TotalHours(d2)
PRINT Zanna.Time.Duration.ToString(d2)

DIM d3 AS INTEGER
LET d3 = Zanna.Time.Duration.Add(d, d2)
PRINT Zanna.Time.Duration.TotalSeconds(d3)

DIM dz AS INTEGER
LET dz = Zanna.Time.Duration.Zero()
PRINT Zanna.Time.Duration.TotalSeconds(dz)

PRINT "done"
END
