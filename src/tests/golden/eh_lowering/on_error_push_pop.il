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
extern @Zanna.Runtime.Unsafe.RaiseKind(i32, i32, i32) -> void
global const str @.L0 = "\n"
func @__mod_init$oop() -> void {
entry:
  ret
}
func @main() -> i64 {
entry:
  call @__mod_init$oop()
  %t0 = alloca 8
  store i32, %t0, 0
  %t1 = alloca 8
  store i32, %t1, 0
  %t2 = alloca 8
  store i32, %t2, 0
  %t3 = alloca 8
  store i32, %t3, 0
  %t4 = alloca 8
  store i32, %t4, 0
  %t5 = alloca 8
  store i32, %t5, 0
  %t6 = alloca 8
  store i32, %t6, 0
  %t7 = alloca 8
  store i64, %t7, 0
  br onerr_arm
L10:
  .loc 1 1 4
  store i32, %t0, 1
  .loc 1 1 4
  br L20
L20:
  .loc 1 2 10
  call @Zanna.Terminal.PrintI64(1)
  .loc 1 2 10
  %t10 = const_str @.L0
  .loc 1 2 10
  call @Zanna.Terminal.PrintStr(%t10)
  .loc 1 2 4
  br L30
L30:
  .loc 1 3 4
  br exit
L100:
  .loc 1 4 11
  call @Zanna.Terminal.PrintI64(2)
  .loc 1 4 11
  %t11 = const_str @.L0
  .loc 1 4 11
  call @Zanna.Terminal.PrintStr(%t11)
  .loc 1 4 5
  br L110
L110:
  .loc 1 5 5
  br exit
exit:
  eh.pop
  ret 0
handler ^onerr_handler(%err:Error, %tok:ResumeTok):
  eh.entry
  %t18 = load i64, %t7
  %t19 = icmp_ne %t18, 0
  cbr %t19, onerr_rethrow(%err, %tok), onerr_select(%err, %tok)
onerr_arm:
  eh.push ^onerr_handler
  %t29 = load i32, %t1
  switch.i32 %t29, ^L10, 1 -> ^L100
handler ^onerr_select(%err:Error, %tok:ResumeTok):
  eh.entry
  %t20 = load i32, %t0
  switch.i32 %t20, ^onerr_enter(%err, %tok), 0 -> ^onerr_rethrow(%err, %tok)
handler ^onerr_enter(%err:Error, %tok:ResumeTok):
  eh.entry
  %t21 = err.get_kind %err
  store i32, %t4, %t21
  %t22 = err.get_code %err
  store i32, %t5, %t22
  %t23 = err.get_line %err
  store i32, %t6, %t23
  store i64, %t7, 1
  %t24 = load i32, %t2
  store i32, %t3, %t24
  %t25 = load i32, %t0
  store i32, %t1, %t25
  resume.label %tok, ^onerr_arm
handler ^onerr_rethrow(%err:Error, %tok:ResumeTok):
  eh.entry
  %t26 = err.get_kind %err
  %t27 = err.get_code %err
  %t28 = err.get_line %err
  call @Zanna.Runtime.Unsafe.RaiseKind(%t26, %t27, %t28)
  trap
}
