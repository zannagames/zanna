il 0.3.0

// Regression: CheckOpt demotes this loop's checked arithmetic to plain add/sub/mul
// because the counter's range bounds every value, and IndVarSimplify then matches
// the plain `19 + i * 211` chain. Strength-reducing that chain into a loop-carried
// parameter erases the range the subtract's demotion rests on, so the pass must
// leave this loop alone. Reduced from Xenoscape's parallax layers (camera.zia).

extern @Zanna.Time.Clock.NowMs() -> i64

func @main() -> i64 {
entry:
  br body(0, 0)
body(%t0:i64, %t1:i64):
  %t2 = call @Zanna.Time.Clock.NowMs()
  %t3 = imul.ovf %t0, 211
  %t4 = iadd.ovf %t3, 19
  %t5 = srem.chk0 %t2, 1600
  %t6 = isub.ovf %t4, %t5
  %t7 = scmp_lt %t6, -20
  cbr %t7, wrap, join(%t0, %t1, %t6)
wrap:
  %t8 = iadd.ovf %t6, 1600
  br join(%t0, %t1, %t8)
join(%t9:i64, %t10:i64, %t11:i64):
  %t12 = iadd.ovf %t10, %t11
  %t13 = iadd.ovf %t9, 1
  %t14 = scmp_lt %t13, 6
  cbr %t14, body(%t13, %t12), done(%t12)
done(%t15:i64):
  ret %t15
}
