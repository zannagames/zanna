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
extern @rt_int_floor(f64) -> f64
extern @rt_val(str) -> f64
extern @rt_val_to_double(ptr, ptr) -> f64
extern @rt_string_cstr(str) -> ptr
extern @rt_str_empty() -> str
extern @Zanna.Core.Convert.ToStringDouble(f64) -> str
extern @Zanna.Core.Convert.ToStringInt(i64) -> str
extern @rt_str_retain_maybe(str) -> void
extern @rt_str_release_maybe(str) -> void
global const str @.L0 = "              "
global const str @.L1 = "\n"
global const str @.L2 = " 42 "
func @__mod_init$oop() -> void {
entry:
  ret
}
func @main() -> i64 {
entry:
  call @__mod_init$oop()
  %a = alloca 8
  %b# = alloca 8
  %s$ = alloca 8
  %t3:str = call @rt_str_empty()
  store str, %s$, %t3
  %x# = alloca 8
  %y# = alloca 8
  br L10
L10:
  .loc 1 1 4
  store i64, %a, 1
  .loc 1 1 4
  br L20
L20:
  .loc 1 2 4
  store f64, %b#, 2.5
  .loc 1 2 4
  br L30
L30:
  .loc 1 3 15
  %t6 = load i64, %a
  .loc 1 3 10
  %t7:str = call @Zanna.Core.Convert.ToStringInt(%t6)
  .loc 1 3 10
  call @Zanna.Terminal.PrintStr(%t7)
  .loc 1 3 10
  %t8 = const_str @.L0
  .loc 1 3 10
  call @Zanna.Terminal.PrintStr(%t8)
  .loc 1 3 24
  %t9 = load f64, %b#
  .loc 1 3 19
  %t10:str = call @Zanna.Core.Convert.ToStringDouble(%t9)
  .loc 1 3 19
  call @Zanna.Terminal.PrintStr(%t10)
  .loc 1 3 19
  %t11 = const_str @.L1
  .loc 1 3 19
  call @Zanna.Terminal.PrintStr(%t11)
  .loc 1 3 4
  call @rt_str_release_maybe(%t7)
  .loc 1 3 4
  call @rt_str_release_maybe(%t10)
  .loc 1 3 4
  br L40
L40:
  .loc 1 4 13
  %t12 = const_str @.L2
  .loc 1 4 4
  call @rt_str_retain_maybe(%t12)
  .loc 1 4 4
  %t13 = load str, %s$
  .loc 1 4 4
  call @rt_str_release_maybe(%t13)
  .loc 1 4 4
  store str, %s$, %t12
  .loc 1 4 4
  br L50
L50:
  .loc 1 5 14
  %t14 = load str, %s$
  .loc 1 5 14
  %t15:ptr = call @rt_string_cstr(%t14)
  .loc 1 5 14
  %t16 = alloca 1
  .loc 1 5 14
  %t17:f64 = call @rt_val_to_double(%t15, %t16)
  .loc 1 5 14
  %t18 = load i1, %t16
  .loc 1 5 14
  cbr %t18, val_ok, val_fail
L60:
  .loc 1 6 4
  store f64, %x#, 1.8999999999999999
  .loc 1 6 4
  br L70
L70:
  .loc 1 7 4
  store f64, %y#, -1.8999999999999999
  .loc 1 7 4
  br L80
L80:
  .loc 1 8 14
  %t23 = load f64, %x#
  .loc 1 8 10
  %t24:f64 = call @rt_int_floor(%t23)
  .loc 1 8 10
  call @Zanna.Terminal.PrintF64(%t24)
  .loc 1 8 10
  %t25 = const_str @.L0
  .loc 1 8 10
  call @Zanna.Terminal.PrintStr(%t25)
  .loc 1 8 23
  %t26 = load f64, %y#
  .loc 1 8 19
  %t27:f64 = call @rt_int_floor(%t26)
  .loc 1 8 19
  call @Zanna.Terminal.PrintF64(%t27)
  .loc 1 8 19
  %t28 = const_str @.L1
  .loc 1 8 19
  call @Zanna.Terminal.PrintStr(%t28)
  .loc 1 8 4
  br L90
L90:
  .loc 1 9 4
  br exit
exit:
  %t29 = load str, %s$
  call @rt_str_release_maybe(%t29)
  ret 0
val_ok:
  .loc 1 5 14
  call @Zanna.Terminal.PrintF64(%t17)
  .loc 1 5 14
  %t22 = const_str @.L1
  .loc 1 5 14
  call @Zanna.Terminal.PrintStr(%t22)
  .loc 1 5 4
  br L60
val_fail:
  .loc 1 5 14
  %t19 = fcmp_ne %t17, %t17
  .loc 1 5 14
  cbr %t19, val_nan, val_over
val_nan:
  .loc 1 5 14
  %t20 = cast.fp_to_si.rte.chk NaN
  .loc 1 5 14
  trap
val_over:
  .loc 1 5 14
  %t21 = cast.fp_to_si.rte.chk 1.7976931348623157e+308
  .loc 1 5 14
  trap
}
