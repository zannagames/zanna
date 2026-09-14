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
extern @Zanna.Core.Convert.ToStringInt(i64) -> str
extern @rt_write_ch_err(i32, str) -> i32
extern @rt_println_ch_err(i32, str) -> i32
global const str @.L0 = "A"
global const str @.L1 = "B"
global const str @.L2 = "C"
func @__mod_init$oop() -> void {
entry:
  ret
}
func @main() -> i64 {
entry:
  call @__mod_init$oop()
  br L10
L10:
  .loc 1 1 4
  %t0:i32 = cast.si_narrow.chk 2
  .loc 1 1 14
  %t1 = const_str @.L0
  .loc 1 1 14
  %t2:i32 = call @rt_write_ch_err(%t0, %t1)
  .loc 1 1 14
  %t3 = alloca 8
  .loc 1 1 14
  store i64, %t3, 0
  .loc 1 1 14
  store i32, %t3, %t2
  .loc 1 1 14
  %t4 = load i64, %t3
  .loc 1 1 14
  %t5 = icmp_ne %t4, 0
  .loc 1 1 14
  cbr %t5, printch_fail, printch_cont
L20:
  .loc 1 2 4
  %t11:i32 = cast.si_narrow.chk 2
  .loc 1 2 14
  %t12 = const_str @.L1
  .loc 1 2 14
  %t13:i32 = call @rt_write_ch_err(%t11, %t12)
  .loc 1 2 14
  %t14 = alloca 8
  .loc 1 2 14
  store i64, %t14, 0
  .loc 1 2 14
  store i32, %t14, %t13
  .loc 1 2 14
  %t15 = load i64, %t14
  .loc 1 2 14
  %t16 = icmp_ne %t15, 0
  .loc 1 2 14
  cbr %t16, printch_fail2, printch_cont2
L30:
  .loc 1 3 4
  %t22:i32 = cast.si_narrow.chk 2
  .loc 1 3 14
  %t23 = const_str @.L2
  .loc 1 3 14
  %t24:i32 = call @rt_println_ch_err(%t22, %t23)
  .loc 1 3 14
  %t25 = alloca 8
  .loc 1 3 14
  store i64, %t25, 0
  .loc 1 3 14
  store i32, %t25, %t24
  .loc 1 3 14
  %t26 = load i64, %t25
  .loc 1 3 14
  %t27 = icmp_ne %t26, 0
  .loc 1 3 14
  cbr %t27, printch_fail4, printch_cont4
L40:
  .loc 1 4 4
  br exit
exit:
  ret 0
printch_fail:
  .loc 1 1 14
  trap.from_err i32 %t2
printch_cont:
  .loc 1 1 19
  %t6:str = call @Zanna.Core.Convert.ToStringInt(42)
  .loc 1 1 19
  %t7:i32 = call @rt_println_ch_err(%t0, %t6)
  .loc 1 1 19
  %t8 = alloca 8
  .loc 1 1 19
  store i64, %t8, 0
  .loc 1 1 19
  store i32, %t8, %t7
  .loc 1 1 19
  %t9 = load i64, %t8
  .loc 1 1 19
  %t10 = icmp_ne %t9, 0
  .loc 1 1 19
  cbr %t10, printch_fail1, printch_cont1
printch_fail1:
  .loc 1 1 19
  trap.from_err i32 %t7
printch_cont1:
  .loc 1 1 4
  br L20
printch_fail2:
  .loc 1 2 14
  trap.from_err i32 %t13
printch_cont2:
  .loc 1 2 19
  %t17:str = call @Zanna.Core.Convert.ToStringInt(7)
  .loc 1 2 19
  %t18:i32 = call @rt_println_ch_err(%t11, %t17)
  .loc 1 2 19
  %t19 = alloca 8
  .loc 1 2 19
  store i64, %t19, 0
  .loc 1 2 19
  store i32, %t19, %t18
  .loc 1 2 19
  %t20 = load i64, %t19
  .loc 1 2 19
  %t21 = icmp_ne %t20, 0
  .loc 1 2 19
  cbr %t21, printch_fail3, printch_cont3
printch_fail3:
  .loc 1 2 19
  trap.from_err i32 %t18
printch_cont3:
  .loc 1 2 4
  br L30
printch_fail4:
  .loc 1 3 14
  trap.from_err i32 %t24
printch_cont4:
  .loc 1 3 4
  br L40
}
