il 0.3.0
extern @Zanna.Terminal.PrintI64(i64) -> void

func @scribble(i64 %n) -> i64 {
entry(%n:i64):
  %p = alloca 64
  %q = gep %p, 8
  %r = gep %p, 56
  store i64, %p, %n
  store i64, %q, %n
  store i64, %r, %n
  %v = load i64, %q
  ret %v
}

func @fresh() -> i64 {
entry:
  %wide = alloca 16
  %tail = alloca 3
  %hi = gep %wide, 8
  %last = gep %tail, 2
  %a = load i64, %wide
  %b = load i64, %hi
  %c = load i1, %last
  %c64 = zext1 %c
  %ab = iadd.ovf %a, %b
  %sum = iadd.ovf %ab, %c64
  ret %sum
}

func @main() -> i64 {
entry:
  %s = call @scribble(123456789)
  %z = call @fresh()
  call @Zanna.Terminal.PrintI64(%z)
  ret %z
}
