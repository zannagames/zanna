' =============================================================================
' API Audit: Zanna.Math.Easing - Easing Functions (BASIC)
' =============================================================================
' Tests all 28 easing functions at t=0.0, t=0.5, t=1.0
' =============================================================================

PRINT "=== API Audit: Zanna.Math.Easing ==="

' --- Linear ---
PRINT "--- Linear ---"
PRINT "Easing.Linear(0.0): "; Zanna.Math.Easing.Linear(0.0)
PRINT "Easing.Linear(0.5): "; Zanna.Math.Easing.Linear(0.5)
PRINT "Easing.Linear(1.0): "; Zanna.Math.Easing.Linear(1.0)

' --- InQuad ---
PRINT "--- InQuad ---"
PRINT "Easing.EaseInQuad(0.0): "; Zanna.Math.Easing.EaseInQuad(0.0)
PRINT "Easing.EaseInQuad(0.5): "; Zanna.Math.Easing.EaseInQuad(0.5)
PRINT "Easing.EaseInQuad(1.0): "; Zanna.Math.Easing.EaseInQuad(1.0)

' --- OutQuad ---
PRINT "--- OutQuad ---"
PRINT "Easing.EaseOutQuad(0.0): "; Zanna.Math.Easing.EaseOutQuad(0.0)
PRINT "Easing.EaseOutQuad(0.5): "; Zanna.Math.Easing.EaseOutQuad(0.5)
PRINT "Easing.EaseOutQuad(1.0): "; Zanna.Math.Easing.EaseOutQuad(1.0)

' --- InOutQuad ---
PRINT "--- InOutQuad ---"
PRINT "Easing.EaseInOutQuad(0.0): "; Zanna.Math.Easing.EaseInOutQuad(0.0)
PRINT "Easing.EaseInOutQuad(0.5): "; Zanna.Math.Easing.EaseInOutQuad(0.5)
PRINT "Easing.EaseInOutQuad(1.0): "; Zanna.Math.Easing.EaseInOutQuad(1.0)

' --- InCubic ---
PRINT "--- InCubic ---"
PRINT "Easing.EaseInCubic(0.0): "; Zanna.Math.Easing.EaseInCubic(0.0)
PRINT "Easing.EaseInCubic(0.5): "; Zanna.Math.Easing.EaseInCubic(0.5)
PRINT "Easing.EaseInCubic(1.0): "; Zanna.Math.Easing.EaseInCubic(1.0)

' --- OutCubic ---
PRINT "--- OutCubic ---"
PRINT "Easing.EaseOutCubic(0.0): "; Zanna.Math.Easing.EaseOutCubic(0.0)
PRINT "Easing.EaseOutCubic(0.5): "; Zanna.Math.Easing.EaseOutCubic(0.5)
PRINT "Easing.EaseOutCubic(1.0): "; Zanna.Math.Easing.EaseOutCubic(1.0)

' --- InOutCubic ---
PRINT "--- InOutCubic ---"
PRINT "Easing.EaseInOutCubic(0.0): "; Zanna.Math.Easing.EaseInOutCubic(0.0)
PRINT "Easing.EaseInOutCubic(0.5): "; Zanna.Math.Easing.EaseInOutCubic(0.5)
PRINT "Easing.EaseInOutCubic(1.0): "; Zanna.Math.Easing.EaseInOutCubic(1.0)

' --- InQuart ---
PRINT "--- InQuart ---"
PRINT "Easing.EaseInQuart(0.0): "; Zanna.Math.Easing.EaseInQuart(0.0)
PRINT "Easing.EaseInQuart(0.5): "; Zanna.Math.Easing.EaseInQuart(0.5)
PRINT "Easing.EaseInQuart(1.0): "; Zanna.Math.Easing.EaseInQuart(1.0)

' --- OutQuart ---
PRINT "--- OutQuart ---"
PRINT "Easing.EaseOutQuart(0.0): "; Zanna.Math.Easing.EaseOutQuart(0.0)
PRINT "Easing.EaseOutQuart(0.5): "; Zanna.Math.Easing.EaseOutQuart(0.5)
PRINT "Easing.EaseOutQuart(1.0): "; Zanna.Math.Easing.EaseOutQuart(1.0)

' --- InOutQuart ---
PRINT "--- InOutQuart ---"
PRINT "Easing.EaseInOutQuart(0.0): "; Zanna.Math.Easing.EaseInOutQuart(0.0)
PRINT "Easing.EaseInOutQuart(0.5): "; Zanna.Math.Easing.EaseInOutQuart(0.5)
PRINT "Easing.EaseInOutQuart(1.0): "; Zanna.Math.Easing.EaseInOutQuart(1.0)

' --- InSine ---
PRINT "--- InSine ---"
PRINT "Easing.EaseInSine(0.0): "; Zanna.Math.Easing.EaseInSine(0.0)
PRINT "Easing.EaseInSine(0.5): "; Zanna.Math.Easing.EaseInSine(0.5)
PRINT "Easing.EaseInSine(1.0): "; Zanna.Math.Easing.EaseInSine(1.0)

' --- OutSine ---
PRINT "--- OutSine ---"
PRINT "Easing.EaseOutSine(0.0): "; Zanna.Math.Easing.EaseOutSine(0.0)
PRINT "Easing.EaseOutSine(0.5): "; Zanna.Math.Easing.EaseOutSine(0.5)
PRINT "Easing.EaseOutSine(1.0): "; Zanna.Math.Easing.EaseOutSine(1.0)

' --- InOutSine ---
PRINT "--- InOutSine ---"
PRINT "Easing.EaseInOutSine(0.0): "; Zanna.Math.Easing.EaseInOutSine(0.0)
PRINT "Easing.EaseInOutSine(0.5): "; Zanna.Math.Easing.EaseInOutSine(0.5)
PRINT "Easing.EaseInOutSine(1.0): "; Zanna.Math.Easing.EaseInOutSine(1.0)

' --- InExpo ---
PRINT "--- InExpo ---"
PRINT "Easing.EaseInExpo(0.0): "; Zanna.Math.Easing.EaseInExpo(0.0)
PRINT "Easing.EaseInExpo(0.5): "; Zanna.Math.Easing.EaseInExpo(0.5)
PRINT "Easing.EaseInExpo(1.0): "; Zanna.Math.Easing.EaseInExpo(1.0)

' --- OutExpo ---
PRINT "--- OutExpo ---"
PRINT "Easing.EaseOutExpo(0.0): "; Zanna.Math.Easing.EaseOutExpo(0.0)
PRINT "Easing.EaseOutExpo(0.5): "; Zanna.Math.Easing.EaseOutExpo(0.5)
PRINT "Easing.EaseOutExpo(1.0): "; Zanna.Math.Easing.EaseOutExpo(1.0)

' --- InOutExpo ---
PRINT "--- InOutExpo ---"
PRINT "Easing.EaseInOutExpo(0.0): "; Zanna.Math.Easing.EaseInOutExpo(0.0)
PRINT "Easing.EaseInOutExpo(0.5): "; Zanna.Math.Easing.EaseInOutExpo(0.5)
PRINT "Easing.EaseInOutExpo(1.0): "; Zanna.Math.Easing.EaseInOutExpo(1.0)

' --- InCirc ---
PRINT "--- InCirc ---"
PRINT "Easing.EaseInCirc(0.0): "; Zanna.Math.Easing.EaseInCirc(0.0)
PRINT "Easing.EaseInCirc(0.5): "; Zanna.Math.Easing.EaseInCirc(0.5)
PRINT "Easing.EaseInCirc(1.0): "; Zanna.Math.Easing.EaseInCirc(1.0)

' --- OutCirc ---
PRINT "--- OutCirc ---"
PRINT "Easing.EaseOutCirc(0.0): "; Zanna.Math.Easing.EaseOutCirc(0.0)
PRINT "Easing.EaseOutCirc(0.5): "; Zanna.Math.Easing.EaseOutCirc(0.5)
PRINT "Easing.EaseOutCirc(1.0): "; Zanna.Math.Easing.EaseOutCirc(1.0)

' --- InOutCirc ---
PRINT "--- InOutCirc ---"
PRINT "Easing.EaseInOutCirc(0.0): "; Zanna.Math.Easing.EaseInOutCirc(0.0)
PRINT "Easing.EaseInOutCirc(0.5): "; Zanna.Math.Easing.EaseInOutCirc(0.5)
PRINT "Easing.EaseInOutCirc(1.0): "; Zanna.Math.Easing.EaseInOutCirc(1.0)

' --- InBack ---
PRINT "--- InBack ---"
PRINT "Easing.EaseInBack(0.0): "; Zanna.Math.Easing.EaseInBack(0.0)
PRINT "Easing.EaseInBack(0.5): "; Zanna.Math.Easing.EaseInBack(0.5)
PRINT "Easing.EaseInBack(1.0): "; Zanna.Math.Easing.EaseInBack(1.0)

' --- OutBack ---
PRINT "--- OutBack ---"
PRINT "Easing.EaseOutBack(0.0): "; Zanna.Math.Easing.EaseOutBack(0.0)
PRINT "Easing.EaseOutBack(0.5): "; Zanna.Math.Easing.EaseOutBack(0.5)
PRINT "Easing.EaseOutBack(1.0): "; Zanna.Math.Easing.EaseOutBack(1.0)

' --- InOutBack ---
PRINT "--- InOutBack ---"
PRINT "Easing.EaseInOutBack(0.0): "; Zanna.Math.Easing.EaseInOutBack(0.0)
PRINT "Easing.EaseInOutBack(0.5): "; Zanna.Math.Easing.EaseInOutBack(0.5)
PRINT "Easing.EaseInOutBack(1.0): "; Zanna.Math.Easing.EaseInOutBack(1.0)

' --- InElastic ---
PRINT "--- InElastic ---"
PRINT "Easing.EaseInElastic(0.0): "; Zanna.Math.Easing.EaseInElastic(0.0)
PRINT "Easing.EaseInElastic(0.5): "; Zanna.Math.Easing.EaseInElastic(0.5)
PRINT "Easing.EaseInElastic(1.0): "; Zanna.Math.Easing.EaseInElastic(1.0)

' --- OutElastic ---
PRINT "--- OutElastic ---"
PRINT "Easing.EaseOutElastic(0.0): "; Zanna.Math.Easing.EaseOutElastic(0.0)
PRINT "Easing.EaseOutElastic(0.5): "; Zanna.Math.Easing.EaseOutElastic(0.5)
PRINT "Easing.EaseOutElastic(1.0): "; Zanna.Math.Easing.EaseOutElastic(1.0)

' --- InOutElastic ---
PRINT "--- InOutElastic ---"
PRINT "Easing.EaseInOutElastic(0.0): "; Zanna.Math.Easing.EaseInOutElastic(0.0)
PRINT "Easing.EaseInOutElastic(0.5): "; Zanna.Math.Easing.EaseInOutElastic(0.5)
PRINT "Easing.EaseInOutElastic(1.0): "; Zanna.Math.Easing.EaseInOutElastic(1.0)

' --- InBounce ---
PRINT "--- InBounce ---"
PRINT "Easing.EaseInBounce(0.0): "; Zanna.Math.Easing.EaseInBounce(0.0)
PRINT "Easing.EaseInBounce(0.5): "; Zanna.Math.Easing.EaseInBounce(0.5)
PRINT "Easing.EaseInBounce(1.0): "; Zanna.Math.Easing.EaseInBounce(1.0)

' --- OutBounce ---
PRINT "--- OutBounce ---"
PRINT "Easing.EaseOutBounce(0.0): "; Zanna.Math.Easing.EaseOutBounce(0.0)
PRINT "Easing.EaseOutBounce(0.5): "; Zanna.Math.Easing.EaseOutBounce(0.5)
PRINT "Easing.EaseOutBounce(1.0): "; Zanna.Math.Easing.EaseOutBounce(1.0)

' --- InOutBounce ---
PRINT "--- InOutBounce ---"
PRINT "Easing.EaseInOutBounce(0.0): "; Zanna.Math.Easing.EaseInOutBounce(0.0)
PRINT "Easing.EaseInOutBounce(0.5): "; Zanna.Math.Easing.EaseInOutBounce(0.5)
PRINT "Easing.EaseInOutBounce(1.0): "; Zanna.Math.Easing.EaseInOutBounce(1.0)

PRINT "=== Easing Audit Complete ==="
END
