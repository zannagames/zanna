il 0.3.0
extern @Zanna.String.Asc(str) -> i64
extern @Zanna.String.ByteAt(str, i64) -> i64
extern @Zanna.String.Chr(i64) -> str
extern @Zanna.String.IndexOf(str, str) -> i64
extern @Zanna.String.IndexOfFrom(str, i64, str) -> i64
extern @Zanna.String.ToLower(str) -> str
extern @Zanna.String.TrimStart(str) -> str
extern @Zanna.String.Left(str, i64) -> str
extern @Zanna.String.get_Length(str) -> i64
extern @Zanna.String.Mid(str, i64) -> str
extern @Zanna.String.MidLen(str, i64, i64) -> str
extern @Zanna.String.TrimEnd(str) -> str
extern @Zanna.String.Right(str, i64) -> str
extern @Zanna.String.Compare(str, str) -> i64
extern @Zanna.String.CompareIgnoreCase(str, str) -> i64
extern @Zanna.String.Count(str, str) -> i64
extern @Zanna.String.EndsWith(str, str) -> i1
extern @Zanna.String.Reverse(str) -> str
extern @Zanna.String.Contains(str, str) -> i1
extern @Zanna.String.PadLeft(str, i64, str) -> str
extern @Zanna.String.PadRight(str, i64, str) -> str
extern @Zanna.String.Repeat(str, i64) -> str
extern @Zanna.String.Replace(str, str, str) -> str
extern @Zanna.String.Split(str, str) -> ptr
extern @Zanna.String.Lines(str) -> ptr
extern @Zanna.Text.Char.IsIdentifierStart(str) -> i1
extern @Zanna.Text.Char.IsIdentifierPart(str) -> i1
extern @Zanna.Text.Char.IsAlphanumeric(str) -> i1
extern @Zanna.String.StartsWith(str, str) -> i1
extern @Zanna.String.Substring(str, i64, i64) -> str
extern @Zanna.String.Trim(str) -> str
extern @Zanna.String.ToUpper(str) -> str
extern @Zanna.String.Join(str, ptr) -> str
extern @Zanna.Terminal.PrintF64(f64) -> void
extern @Zanna.Terminal.PrintI64(i64) -> void
extern @Zanna.Terminal.PrintStr(str) -> void
extern @rt_str_empty() -> str
extern @rt_abs_i64(i64) -> i64
extern @rt_abs_f64(f64) -> f64
extern @rt_floor(f64) -> f64
extern @rt_ceil(f64) -> f64
extern @rt_sqrt(f64) -> f64
global const str @.L0 = "\n"
func @__mod_init$oop() -> void {
entry:
  ret
}
func @main() -> i64 {
entry:
  call @__mod_init$oop()
  br L10
L10:
  .loc 1 1 10
  %t0:i64 = call @rt_abs_i64(-5)
  .loc 1 1 10
  call @Zanna.Terminal.PrintI64(%t0)
  .loc 1 1 10
  %t1 = const_str @.L0
  .loc 1 1 10
  call @Zanna.Terminal.PrintStr(%t1)
  .loc 1 1 4
  br L20
L20:
  .loc 1 2 10
  %t2:f64 = call @rt_abs_f64(-1.5)
  .loc 1 2 10
  call @Zanna.Terminal.PrintF64(%t2)
  .loc 1 2 10
  %t3 = const_str @.L0
  .loc 1 2 10
  call @Zanna.Terminal.PrintStr(%t3)
  .loc 1 2 4
  br L30
L30:
  .loc 1 3 10
  %t4:f64 = call @rt_floor(1.8999999999999999)
  .loc 1 3 10
  call @Zanna.Terminal.PrintF64(%t4)
  .loc 1 3 10
  %t5 = const_str @.L0
  .loc 1 3 10
  call @Zanna.Terminal.PrintStr(%t5)
  .loc 1 3 4
  br L40
L40:
  .loc 1 4 10
  %t6:f64 = call @rt_ceil(1.1000000000000001)
  .loc 1 4 10
  call @Zanna.Terminal.PrintF64(%t6)
  .loc 1 4 10
  %t7 = const_str @.L0
  .loc 1 4 10
  call @Zanna.Terminal.PrintStr(%t7)
  .loc 1 4 4
  br L50
L50:
  .loc 1 5 10
  %t8:f64 = call @rt_sqrt(9.0)
  .loc 1 5 10
  call @Zanna.Terminal.PrintF64(%t8)
  .loc 1 5 10
  %t9 = const_str @.L0
  .loc 1 5 10
  call @Zanna.Terminal.PrintStr(%t9)
  .loc 1 5 4
  br exit
exit:
  ret 0
}
