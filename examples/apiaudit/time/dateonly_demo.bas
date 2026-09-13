' =============================================================================
' API Audit: Zanna.Time.DateOnly (BASIC)
' =============================================================================
' Tests: Create, Today, Parse, FromDays, Year, Month, Day, DayOfWeek,
'        DayOfYear, ToDays, AddDays, AddMonths, AddYears, DiffDays,
'        IsLeapYear, DaysInMonth, StartOfMonth, EndOfMonth, StartOfYear,
'        EndOfYear, Cmp, Equals, ToString, Format
' =============================================================================

PRINT "=== API Audit: Zanna.Time.DateOnly ==="

' --- Create ---
PRINT "--- Create ---"
DIM d AS OBJECT = Zanna.Time.DateOnly.FromParts(2024, 6, 15)
PRINT "Created 2024-06-15"

' --- Year / Month / Day ---
PRINT "--- Year / Month / Day ---"
PRINT d.Year
PRINT d.Month
PRINT d.Day

' --- DayOfWeek ---
PRINT "--- DayOfWeek ---"
PRINT d.DayOfWeek

' --- DayOfYear ---
PRINT "--- DayOfYear ---"
PRINT d.DayOfYear

' --- ToString ---
PRINT "--- ToString ---"
PRINT d.ToString()

' --- Format ---
PRINT "--- Format ---"
PRINT d.Format("%Y/%m/%d")

' --- ToDays ---
PRINT "--- ToDays ---"
DIM days AS INTEGER = d.ToDays()
PRINT days

' --- FromDays (roundtrip) ---
PRINT "--- FromDays ---"
DIM d2 AS OBJECT = Zanna.Time.DateOnly.FromDays(days)
PRINT d2.ToString()

' --- Parse ---
PRINT "--- Parse ---"
DIM d3 AS OBJECT = Zanna.Time.DateOnly.Parse("2024-01-01")
PRINT d3.ToString()
PRINT d3.Year
PRINT d3.Month
PRINT d3.Day

' --- AddDays ---
PRINT "--- AddDays ---"
DIM d4 AS OBJECT = d.AddDays(10)
PRINT d4.ToString()

' --- AddDays (negative) ---
PRINT "--- AddDays (negative) ---"
DIM d5 AS OBJECT = d.AddDays(-15)
PRINT d5.ToString()

' --- AddMonths ---
PRINT "--- AddMonths ---"
DIM d6 AS OBJECT = d.AddMonths(3)
PRINT d6.ToString()

' --- AddYears ---
PRINT "--- AddYears ---"
DIM d7 AS OBJECT = d.AddYears(1)
PRINT d7.ToString()

' --- DiffDays ---
PRINT "--- DiffDays ---"
DIM jan1 AS OBJECT = Zanna.Time.DateOnly.FromParts(2024, 1, 1)
PRINT d.DiffDays(jan1)

' --- IsLeapYear ---
PRINT "--- IsLeapYear ---"
PRINT d.IsLeapYear
DIM d2023 AS OBJECT = Zanna.Time.DateOnly.FromParts(2023, 3, 1)
PRINT d2023.IsLeapYear

' --- DaysInMonth ---
PRINT "--- DaysInMonth ---"
PRINT d.DaysInMonth
DIM feb AS OBJECT = Zanna.Time.DateOnly.FromParts(2024, 2, 1)
PRINT feb.DaysInMonth

' --- StartOfMonth ---
PRINT "--- StartOfMonth ---"
PRINT d.StartOfMonth().ToString()

' --- EndOfMonth ---
PRINT "--- EndOfMonth ---"
PRINT d.EndOfMonth().ToString()

' --- StartOfYear ---
PRINT "--- StartOfYear ---"
PRINT d.StartOfYear().ToString()

' --- EndOfYear ---
PRINT "--- EndOfYear ---"
PRINT d.EndOfYear().ToString()

' --- Compare ---
PRINT "--- Compare ---"
DIM later AS OBJECT = Zanna.Time.DateOnly.FromParts(2024, 12, 25)
PRINT d.Compare(later)
PRINT later.Compare(d)
PRINT d.Compare(d)

' --- Equals ---
PRINT "--- Equals ---"
DIM same AS OBJECT = Zanna.Time.DateOnly.FromParts(2024, 6, 15)
PRINT d.Equals(same)
PRINT d.Equals(later)

' --- Today ---
PRINT "--- Today ---"
DIM today AS OBJECT = Zanna.Time.DateOnly.Today()
PRINT "Today: "; today.ToString()
PRINT "Year > 2020: "; today.Year > 2020

PRINT "=== DateOnly Audit Complete ==="
END
