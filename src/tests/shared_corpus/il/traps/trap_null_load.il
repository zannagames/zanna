il 0.3.0

func @main() -> i64 {
entry:
  %p = const_null
  %q = gep %p, 24
  %v = load i64, %q
  ret %v
}
