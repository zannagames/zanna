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
extern @Zanna.Core.Diagnostics.Trap(str) -> void
global const str @.L0 = "gosub: stack overflow"
global const str @.L1 = "Back"
global const str @.L2 = "\n"
global const str @.L3 = "Done"
global const str @.L4 = "First"
global const str @.L5 = "After Inner"
global const str @.L6 = "gosub: empty return stack"
global const str @.L7 = "Second"
global const str @.L8 = "Inner"
func @__mod_init$oop() -> void {
entry:
  ret
}
func @main() -> i64 {
entry:
  call @__mod_init$oop()
  %a = alloca 8
  %b = alloca 8
  %t2 = alloca 8
  %t3 = alloca 512
  store i64, %t2, 0
  br L10
L10:
  .loc 1 1 4
  store i64, %a, 1
  .loc 1 1 4
  br L20
L20:
  .loc 1 2 4
  store i64, %b, 2
  .loc 1 2 4
  br L30
L30:
  .loc 1 3 4
  %t4 = load i64, %t2
  .loc 1 3 4
  %t5 = scmp_ge %t4, 128
  .loc 1 3 4
  cbr %t5, gosub_overflow, gosub_push
L40:
  .loc 1 4 10
  %t10 = const_str @.L1
  .loc 1 4 10
  call @Zanna.Terminal.PrintStr(%t10)
  .loc 1 4 10
  %t11 = const_str @.L2
  .loc 1 4 10
  call @Zanna.Terminal.PrintStr(%t11)
  .loc 1 4 4
  br L50
L50:
  .loc 1 5 4
  %t12 = load i64, %t2
  .loc 1 5 4
  %t13 = scmp_ge %t12, 128
  .loc 1 5 4
  cbr %t13, gosub_overflow1, gosub_push1
L60:
  .loc 1 6 10
  %t18 = const_str @.L3
  .loc 1 6 10
  call @Zanna.Terminal.PrintStr(%t18)
  .loc 1 6 10
  %t19 = const_str @.L2
  .loc 1 6 10
  call @Zanna.Terminal.PrintStr(%t19)
  .loc 1 6 4
  br L70
L70:
  .loc 1 7 4
  ret 0
L200:
  .loc 1 8 11
  %t20 = const_str @.L4
  .loc 1 8 11
  call @Zanna.Terminal.PrintStr(%t20)
  .loc 1 8 11
  %t21 = const_str @.L2
  .loc 1 8 11
  call @Zanna.Terminal.PrintStr(%t21)
  .loc 1 8 5
  br L210
L210:
  .loc 1 9 5
  %t22 = load i64, %t2
  .loc 1 9 5
  %t23 = scmp_ge %t22, 128
  .loc 1 9 5
  cbr %t23, gosub_overflow2, gosub_push2
L220:
  .loc 1 10 11
  %t28 = const_str @.L5
  .loc 1 10 11
  call @Zanna.Terminal.PrintStr(%t28)
  .loc 1 10 11
  %t29 = const_str @.L2
  .loc 1 10 11
  call @Zanna.Terminal.PrintStr(%t29)
  .loc 1 10 5
  br L230
L230:
  .loc 1 11 5
  %t30 = load i64, %t2
  .loc 1 11 5
  %t31 = icmp_eq %t30, 0
  .loc 1 11 5
  cbr %t31, gosub_ret_empty, gosub_ret_cont
L300:
  .loc 1 12 11
  %t37 = const_str @.L7
  .loc 1 12 11
  call @Zanna.Terminal.PrintStr(%t37)
  .loc 1 12 11
  %t38 = const_str @.L2
  .loc 1 12 11
  call @Zanna.Terminal.PrintStr(%t38)
  .loc 1 12 5
  br L310
L310:
  .loc 1 13 5
  %t39 = load i64, %t2
  .loc 1 13 5
  %t40 = icmp_eq %t39, 0
  .loc 1 13 5
  cbr %t40, gosub_ret_empty1, gosub_ret_cont1
L400:
  .loc 1 14 11
  %t46 = const_str @.L8
  .loc 1 14 11
  call @Zanna.Terminal.PrintStr(%t46)
  .loc 1 14 11
  %t47 = const_str @.L2
  .loc 1 14 11
  call @Zanna.Terminal.PrintStr(%t47)
  .loc 1 14 5
  br L410
L410:
  .loc 1 15 5
  %t48 = load i64, %t2
  .loc 1 15 5
  %t49 = icmp_eq %t48, 0
  .loc 1 15 5
  cbr %t49, gosub_ret_empty2, gosub_ret_cont2
exit:
  ret 0
gosub_overflow:
  .loc 1 3 4
  %t6 = const_str @.L0
  .loc 1 3 4
  call @Zanna.Core.Diagnostics.Trap(%t6)
  .loc 1 3 4
  trap
gosub_push:
  .loc 1 3 4
  %t7 = imul.ovf %t4, 4
  .loc 1 3 4
  %t8 = gep %t3, %t7
  .loc 1 3 4
  store i32, %t8, 0
  .loc 1 3 4
  %t9 = iadd.ovf %t4, 1
  .loc 1 3 4
  store i64, %t2, %t9
  .loc 1 3 4
  br L200
gosub_overflow1:
  .loc 1 5 4
  %t14 = const_str @.L0
  .loc 1 5 4
  call @Zanna.Core.Diagnostics.Trap(%t14)
  .loc 1 5 4
  trap
gosub_push1:
  .loc 1 5 4
  %t15 = imul.ovf %t12, 4
  .loc 1 5 4
  %t16 = gep %t3, %t15
  .loc 1 5 4
  store i32, %t16, 1
  .loc 1 5 4
  %t17 = iadd.ovf %t12, 1
  .loc 1 5 4
  store i64, %t2, %t17
  .loc 1 5 4
  br L300
gosub_overflow2:
  .loc 1 9 5
  %t24 = const_str @.L0
  .loc 1 9 5
  call @Zanna.Core.Diagnostics.Trap(%t24)
  .loc 1 9 5
  trap
gosub_push2:
  .loc 1 9 5
  %t25 = imul.ovf %t22, 4
  .loc 1 9 5
  %t26 = gep %t3, %t25
  .loc 1 9 5
  store i32, %t26, 2
  .loc 1 9 5
  %t27 = iadd.ovf %t22, 1
  .loc 1 9 5
  store i64, %t2, %t27
  .loc 1 9 5
  br L400
gosub_ret_empty:
  .loc 1 11 5
  %t32 = const_str @.L6
  .loc 1 11 5
  call @Zanna.Core.Diagnostics.Trap(%t32)
  .loc 1 11 5
  trap
gosub_ret_cont:
  .loc 1 11 5
  %t33 = isub.ovf %t30, 1
  .loc 1 11 5
  store i64, %t2, %t33
  .loc 1 11 5
  %t34 = imul.ovf %t33, 4
  .loc 1 11 5
  %t35 = gep %t3, %t34
  .loc 1 11 5
  %t36 = load i32, %t35
  .loc 1 11 5
  switch.i32 %t36, ^gosub_ret_invalid, 0 -> ^L40, 1 -> ^L60, 2 -> ^L220
gosub_ret_invalid:
  .loc 1 11 5
  trap
gosub_ret_empty1:
  .loc 1 13 5
  %t41 = const_str @.L6
  .loc 1 13 5
  call @Zanna.Core.Diagnostics.Trap(%t41)
  .loc 1 13 5
  trap
gosub_ret_cont1:
  .loc 1 13 5
  %t42 = isub.ovf %t39, 1
  .loc 1 13 5
  store i64, %t2, %t42
  .loc 1 13 5
  %t43 = imul.ovf %t42, 4
  .loc 1 13 5
  %t44 = gep %t3, %t43
  .loc 1 13 5
  %t45 = load i32, %t44
  .loc 1 13 5
  switch.i32 %t45, ^gosub_ret_invalid1, 0 -> ^L40, 1 -> ^L60, 2 -> ^L220
gosub_ret_invalid1:
  .loc 1 13 5
  trap
gosub_ret_empty2:
  .loc 1 15 5
  %t50 = const_str @.L6
  .loc 1 15 5
  call @Zanna.Core.Diagnostics.Trap(%t50)
  .loc 1 15 5
  trap
gosub_ret_cont2:
  .loc 1 15 5
  %t51 = isub.ovf %t48, 1
  .loc 1 15 5
  store i64, %t2, %t51
  .loc 1 15 5
  %t52 = imul.ovf %t51, 4
  .loc 1 15 5
  %t53 = gep %t3, %t52
  .loc 1 15 5
  %t54 = load i32, %t53
  .loc 1 15 5
  switch.i32 %t54, ^gosub_ret_invalid2, 0 -> ^L40, 1 -> ^L60, 2 -> ^L220
gosub_ret_invalid2:
  .loc 1 15 5
  trap
}
