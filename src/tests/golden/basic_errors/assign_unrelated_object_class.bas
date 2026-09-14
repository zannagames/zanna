REM A variable declared AS a class does not take an object of an unrelated class. The mismatch
REM is B2001 at compile time instead of a trap at the first use of the value.
DIM names AS Zanna.Collections.Seq
names = Zanna.Collections.Map.New()
END
