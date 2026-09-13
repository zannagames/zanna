' =============================================================================
' API Audit: Zanna.Time.DateRange - Date Range Operations
' =============================================================================
' Tests: New, Start, End, Contains, Overlaps, Intersection, Union, Days,
'        Hours, Duration, ToString
' =============================================================================

PRINT "=== API Audit: Zanna.Time.DateRange ==="

DIM start1 AS INTEGER
DIM end1 AS INTEGER
DIM start2 AS INTEGER
DIM end2 AS INTEGER
start1 = Zanna.Time.DateTime.FromParts(2024, 1, 1, 0, 0, 0)
end1 = Zanna.Time.DateTime.FromParts(2024, 1, 31, 23, 59, 59)
start2 = Zanna.Time.DateTime.FromParts(2024, 1, 15, 0, 0, 0)
end2 = Zanna.Time.DateTime.FromParts(2024, 2, 15, 23, 59, 59)

' --- New ---
PRINT "--- New ---"
DIM r1 AS OBJECT
DIM r2 AS OBJECT
r1 = Zanna.Time.DateRange.New(start1, end1)
PRINT "Range 1: Jan 1 - Jan 31"
r2 = Zanna.Time.DateRange.New(start2, end2)
PRINT "Range 2: Jan 15 - Feb 15"

' --- Start / End ---
PRINT "--- Start / End ---"
PRINT "r1 Start ISO: "; Zanna.Time.DateTime.ToIso8601(Zanna.Time.DateRange.get_Start(r1))
PRINT "r1 End ISO: "; Zanna.Time.DateTime.ToIso8601(Zanna.Time.DateRange.get_End(r1))

' --- Contains ---
PRINT "--- Contains ---"
DIM midJan AS INTEGER
midJan = Zanna.Time.DateTime.FromParts(2024, 1, 15, 12, 0, 0)
DIM midFeb AS INTEGER
midFeb = Zanna.Time.DateTime.FromParts(2024, 2, 15, 12, 0, 0)
PRINT "r1 Contains(Jan 15): "; Zanna.Time.DateRange.Contains(r1, midJan)
PRINT "r1 Contains(Feb 15): "; Zanna.Time.DateRange.Contains(r1, midFeb)

' --- Overlaps ---
PRINT "--- Overlaps ---"
PRINT "r1 Overlaps r2: "; Zanna.Time.DateRange.Overlaps(r1, r2)

' --- Intersection ---
PRINT "--- Intersection ---"
DIM inter AS OBJECT
inter = Zanna.Time.DateRange.Intersection(r1, r2)
PRINT "Intersection Start: "; Zanna.Time.DateTime.ToIso8601(Zanna.Time.DateRange.get_Start(inter))
PRINT "Intersection End: "; Zanna.Time.DateTime.ToIso8601(Zanna.Time.DateRange.get_End(inter))

' --- Union ---
PRINT "--- Union ---"
DIM uni AS OBJECT
uni = Zanna.Time.DateRange.Union(r1, r2)
PRINT "Union Start: "; Zanna.Time.DateTime.ToIso8601(Zanna.Time.DateRange.get_Start(uni))
PRINT "Union End: "; Zanna.Time.DateTime.ToIso8601(Zanna.Time.DateRange.get_End(uni))

' --- Days ---
PRINT "--- Days ---"
PRINT "r1 Days: "; Zanna.Time.DateRange.Days(r1)

' --- Hours ---
PRINT "--- Hours ---"
PRINT "r1 Hours: "; Zanna.Time.DateRange.Hours(r1)

' --- Duration ---
PRINT "--- Duration ---"
PRINT "r1 Duration: "; Zanna.Time.DateRange.Duration(r1)

' --- ToString ---
PRINT "--- ToString ---"
PRINT "r1 ToString: "; Zanna.Time.DateRange.ToString(r1)

PRINT "=== DateRange Demo Complete ==="
END
