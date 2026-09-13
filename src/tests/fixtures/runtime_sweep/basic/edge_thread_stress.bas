' Multi-threaded stress test for collections
' Tests for race conditions and thread safety

DIM counter AS Zanna.Threads.SafeI64
DIM startGate AS Zanna.Threads.Barrier
DIM endGate AS Zanna.Threads.Barrier
DIM errCount AS Zanna.Threads.SafeI64

CONST NUM_THREADS AS INTEGER = 4
CONST ITERS_PER_THREAD AS INTEGER = 1000

counter = Zanna.Threads.SafeI64.New(0)
errCount = Zanna.Threads.SafeI64.New(0)
startGate = Zanna.Threads.Barrier.New(NUM_THREADS + 1)
endGate = Zanna.Threads.Barrier.New(NUM_THREADS + 1)

PRINT "=== Multi-threaded Stress Test ==="
PRINT "Threads: "; NUM_THREADS
PRINT "Iterations per thread: "; ITERS_PER_THREAD
PRINT ""

' Worker function that increments counter
SUB Worker()
    DIM i AS INTEGER
    ' Wait for all threads to start together
    startGate.Arrive()

    ' Increment counter ITERS times
    FOR i = 1 TO ITERS_PER_THREAD
        counter.Add(1)
    NEXT i

    ' Signal completion
    endGate.Arrive()
END SUB

' Start worker threads
DIM threads(NUM_THREADS) AS Zanna.Threads.Thread
DIM t AS INTEGER
FOR t = 1 TO NUM_THREADS
    DIM th AS Zanna.Threads.Thread
    th = Zanna.Threads.Thread.Start(ADDRESSOF Worker, NOTHING)
    threads(t) = th
    PRINT "Started thread "; threads(t).Id
NEXT t

' Release all threads at once
PRINT "Releasing threads..."
startGate.Arrive()

' Wait for all threads to finish
endGate.Arrive()
PRINT "All threads completed"

' Verify counter value
DIM expected AS INTEGER
expected = NUM_THREADS * ITERS_PER_THREAD
PRINT ""
PRINT "Expected counter: "; expected
PRINT "Actual counter: "; counter.Get()

IF counter.Get() = expected THEN
    PRINT "PASS: Counter matches expected value"
ELSE
    PRINT "FAIL: Counter mismatch - possible race condition!"
END IF

' Join all threads
FOR t = 1 TO NUM_THREADS
    threads(t).Join()
NEXT t

PRINT ""
PRINT "=== Stress Test Complete ==="
END
