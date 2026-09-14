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
extern @Zanna.Core.Diagnostics.Trap(str) -> void
global const str @.L0 = "RESUME without error"
global const str @.L1 = "RESUME: no failed statement to retry"
global const str @.L2 = "RESUME NEXT: no failed statement to continue after"
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
  .loc 1 2 4
  store i32, %t0, 2
  .loc 1 2 4
  br L30
L30:
  .loc 1 3 4
  store i32, %t0, 3
  .loc 1 3 4
  br L40
L40:
  .loc 1 4 4
  br exit
L100:
  .loc 1 5 5
  store i32, %t2, 1
  .loc 1 5 5
  %t10 = load i64, %t7
  .loc 1 5 5
  %t11 = icmp_eq %t10, 0
  .loc 1 5 5
  cbr %t11, resume_without_error, resume_ok
L110:
  .loc 1 6 5
  br exit
L200:
  .loc 1 7 5
  store i32, %t2, 2
  .loc 1 7 5
  %t13 = load i64, %t7
  .loc 1 7 5
  %t14 = icmp_eq %t13, 0
  .loc 1 7 5
  cbr %t14, resume_without_error1, resume_ok1
L210:
  .loc 1 8 5
  br exit
L300:
  .loc 1 9 5
  store i32, %t2, 3
  .loc 1 9 5
  %t16 = load i64, %t7
  .loc 1 9 5
  %t17 = icmp_eq %t16, 0
  .loc 1 9 5
  cbr %t17, resume_without_error2, resume_ok2
L310:
  .loc 1 10 5
  br exit
L400:
  .loc 1 11 5
  br exit
exit:
  eh.pop
  ret 0
handler ^onerr_handler(%err:Error, %tok:ResumeTok):
  eh.entry
  %t25 = load i64, %t7
  %t26 = icmp_ne %t25, 0
  cbr %t26, onerr_rethrow(%err, %tok), onerr_select(%err, %tok)
onerr_arm:
  eh.push ^onerr_handler
  %t40 = load i32, %t1
  switch.i32 %t40, ^L10, 1 -> ^L100, 2 -> ^L200, 3 -> ^L300, 4 -> ^resume_dispatch, 5 -> ^resume_next_dispatch, 6 -> ^L400
resume_without_error:
  .loc 1 5 5
  %t12 = const_str @.L0
  .loc 1 5 5
  call @Zanna.Core.Diagnostics.Trap(%t12)
  .loc 1 5 5
  trap
resume_ok:
  .loc 1 5 5
  store i64, %t7, 0
  .loc 1 5 5
  store i32, %t5, 0
  .loc 1 5 5
  store i32, %t1, 4
  .loc 1 5 5
  eh.pop
  .loc 1 5 5
  br onerr_arm
resume_dispatch:
  %t37 = load i32, %t3
  switch.i32 %t37, ^resume_invalid, 1 -> ^L100, 2 -> ^L200, 3 -> ^L300
resume_next:
  .loc 1 5 5
  br L110
resume_without_error1:
  .loc 1 7 5
  %t15 = const_str @.L0
  .loc 1 7 5
  call @Zanna.Core.Diagnostics.Trap(%t15)
  .loc 1 7 5
  trap
resume_ok1:
  .loc 1 7 5
  store i64, %t7, 0
  .loc 1 7 5
  store i32, %t5, 0
  .loc 1 7 5
  store i32, %t1, 5
  .loc 1 7 5
  eh.pop
  .loc 1 7 5
  br onerr_arm
resume_next_dispatch:
  %t39 = load i32, %t3
  switch.i32 %t39, ^resume_next_invalid, 1 -> ^resume_next, 2 -> ^resume_next1, 3 -> ^resume_next2
resume_next1:
  .loc 1 7 5
  br L210
resume_without_error2:
  .loc 1 9 5
  %t18 = const_str @.L0
  .loc 1 9 5
  call @Zanna.Core.Diagnostics.Trap(%t18)
  .loc 1 9 5
  trap
resume_ok2:
  .loc 1 9 5
  store i64, %t7, 0
  .loc 1 9 5
  store i32, %t5, 0
  .loc 1 9 5
  store i32, %t1, 6
  .loc 1 9 5
  eh.pop
  .loc 1 9 5
  br onerr_arm
resume_next2:
  .loc 1 9 5
  br L310
handler ^onerr_select(%err:Error, %tok:ResumeTok):
  eh.entry
  %t27 = load i32, %t0
  switch.i32 %t27, ^onerr_enter(%err, %tok), 0 -> ^onerr_rethrow(%err, %tok)
handler ^onerr_enter(%err:Error, %tok:ResumeTok):
  eh.entry
  %t28 = err.get_kind %err
  store i32, %t4, %t28
  %t29 = err.get_code %err
  store i32, %t5, %t29
  %t30 = err.get_line %err
  store i32, %t6, %t30
  store i64, %t7, 1
  %t31 = load i32, %t2
  store i32, %t3, %t31
  %t32 = load i32, %t0
  store i32, %t1, %t32
  resume.label %tok, ^onerr_arm
handler ^onerr_rethrow(%err:Error, %tok:ResumeTok):
  eh.entry
  %t33 = err.get_kind %err
  %t34 = err.get_code %err
  %t35 = err.get_line %err
  call @Zanna.Runtime.Unsafe.RaiseKind(%t33, %t34, %t35)
  trap
resume_invalid:
  %t36 = const_str @.L1
  call @Zanna.Core.Diagnostics.Trap(%t36)
  trap
resume_next_invalid:
  %t38 = const_str @.L2
  call @Zanna.Core.Diagnostics.Trap(%t38)
  trap
}
