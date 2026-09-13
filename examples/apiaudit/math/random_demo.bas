' =============================================================================
' API Audit: Zanna.Math.Random - Random Number Generation (BASIC)
' =============================================================================
' Tests: Seed, Next, NextInt, Range, Dice, Chance, Gaussian, Exponential
' Note: Random outputs are non-deterministic, so we just verify no crashes.
' =============================================================================

PRINT "=== API Audit: Zanna.Math.Random ==="

' --- Seed ---
PRINT "--- Seed ---"
Zanna.Math.Random.Seed(42)
PRINT "Seeded with 42"

' --- Next ---
PRINT "--- Next ---"
PRINT "Random.NextDouble() [0.0..1.0]: "; Zanna.Math.Random.NextDouble()
PRINT "Random.NextDouble() [0.0..1.0]: "; Zanna.Math.Random.NextDouble()
PRINT "Random.NextDouble() [0.0..1.0]: "; Zanna.Math.Random.NextDouble()

' --- NextInt ---
PRINT "--- NextInt ---"
PRINT "Random.NextInt(100): "; Zanna.Math.Random.NextInt(100)
PRINT "Random.NextInt(100): "; Zanna.Math.Random.NextInt(100)
PRINT "Random.NextInt(10): "; Zanna.Math.Random.NextInt(10)
PRINT "Random.NextInt(1): "; Zanna.Math.Random.NextInt(1)

' --- Range ---
PRINT "--- Range ---"
PRINT "Random.Range(1, 6): "; Zanna.Math.Random.Range(1, 6)
PRINT "Random.Range(1, 6): "; Zanna.Math.Random.Range(1, 6)
PRINT "Random.Range(10, 20): "; Zanna.Math.Random.Range(10, 20)
PRINT "Random.Range(-5, 5): "; Zanna.Math.Random.Range(-5, 5)

' --- Dice ---
PRINT "--- Dice ---"
PRINT "Random.Dice(6): "; Zanna.Math.Random.Dice(6)
PRINT "Random.Dice(6): "; Zanna.Math.Random.Dice(6)
PRINT "Random.Dice(20): "; Zanna.Math.Random.Dice(20)

' --- Chance ---
PRINT "--- Chance ---"
PRINT "Random.Chance(1.0) [always true]: "; Zanna.Math.Random.Chance(1.0)
PRINT "Random.Chance(0.0) [always false]: "; Zanna.Math.Random.Chance(0.0)
PRINT "Random.Chance(0.5): "; Zanna.Math.Random.Chance(0.5)

' --- Gaussian ---
PRINT "--- Gaussian ---"
PRINT "Random.Gaussian(0.0, 1.0): "; Zanna.Math.Random.Gaussian(0.0, 1.0)
PRINT "Random.Gaussian(0.0, 1.0): "; Zanna.Math.Random.Gaussian(0.0, 1.0)
PRINT "Random.Gaussian(100.0, 15.0): "; Zanna.Math.Random.Gaussian(100.0, 15.0)

' --- Exponential ---
PRINT "--- Exponential ---"
PRINT "Random.Exponential(1.0): "; Zanna.Math.Random.Exponential(1.0)
PRINT "Random.Exponential(1.0): "; Zanna.Math.Random.Exponential(1.0)
PRINT "Random.Exponential(0.5): "; Zanna.Math.Random.Exponential(0.5)

PRINT "=== Random Audit Complete ==="
END
