' test_time_extra.bas — Stopwatch, Clock, Countdown, DateOnly, DateRange, RelativeTime
DIM sw AS Zanna.Time.Stopwatch
sw = Zanna.Time.Stopwatch.New()
PRINT "sw running: "; sw.IsRunning
sw.Start()
PRINT "sw running after start: "; sw.IsRunning
sw.Stop()
PRINT "sw running after stop: "; sw.IsRunning
PRINT "sw elapsed ms >= 0: "; (sw.ElapsedMs >= 0)
sw.Reset()
PRINT "sw elapsed after reset: "; sw.ElapsedMs

PRINT "clock ticks > 0: "; (Zanna.Time.Clock.NowMs() > 0)
PRINT "clock ticksus > 0: "; (Zanna.Time.Clock.NowMicros() > 0)

DIM cd AS Zanna.Time.Countdown
cd = Zanna.Time.Countdown.New(5000)
PRINT "cd interval: "; cd.Interval
PRINT "cd expired: "; cd.IsExpired
PRINT "cd running: "; cd.IsRunning

DIM d AS OBJECT
PRINT "relative: "; Zanna.Time.RelativeTime.FormatDuration(65000)
d = Zanna.Time.DateOnly.FromParts(2026, 4, 6)
PRINT "date year: "; d.Year
PRINT "date month: "; d.Month
PRINT "date day: "; d.Day

PRINT "done"
END
