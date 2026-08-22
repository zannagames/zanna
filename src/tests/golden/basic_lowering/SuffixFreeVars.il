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
extern @rt_arr_i64_new(i64) -> ptr
extern @rt_arr_i64_len(ptr) -> i64
extern @rt_arr_i64_get(ptr, i64) -> i64
extern @rt_arr_i64_set(ptr, i64, i64) -> void
extern @rt_arr_i64_retain(ptr) -> void
extern @rt_arr_i64_release(ptr) -> void
extern @rt_arr_oob_panic(i64, i64) -> void
extern @rt_str_retain_maybe(str) -> void
extern @rt_str_release_maybe(str) -> void
global const str @.L0 = "ok"
global const str @.L1 = "\n"
func @__mod_init$oop() -> void {
entry:
  ret
}
func @main() -> i64 {
entry:
  call @__mod_init$oop()
  %count = alloca 8
  %message = alloca 8
  %t2:str = call @rt_str_empty()
  store str, %message, %t2
  %values = alloca 8
  store ptr, %values, null
  %t4 = alloca 8
  br UL1000000000
UL1000000000:
  .loc 1 1 1
  br UL999999999
UL999999999:
  .loc 1 2 1
  br UL999999998
UL999999998:
  .loc 1 3 1
  %t5:ptr = call @rt_arr_i64_new(3)
  .loc 1 3 1
  call @rt_arr_i64_retain(%t5)
  .loc 1 3 1
  %t6 = load ptr, %values
  .loc 1 3 1
  call @rt_arr_i64_release(%t6)
  .loc 1 3 1
  store ptr, %values, %t5
  .loc 1 3 1
  store i64, %t4, 3
  .loc 1 3 1
  br UL999999997
UL999999997:
  .loc 1 4 15
  %t7 = const_str @.L0
  .loc 1 4 1
  call @rt_str_retain_maybe(%t7)
  .loc 1 4 1
  %t8 = load str, %message
  .loc 1 4 1
  call @rt_str_release_maybe(%t8)
  .loc 1 4 1
  store str, %message, %t7
  .loc 1 4 1
  br UL999999996
UL999999996:
  .loc 1 5 1
  store i64, %count, 5
  .loc 1 5 1
  br UL999999995
UL999999995:
  .loc 1 6 17
  %t9 = load i64, %count
  .loc 1 6 1
  %t10 = load ptr, %values
  .loc 1 6 5
  %t11:i64 = call @rt_arr_i64_len(%t10)
  .loc 1 6 5
  %t12 = scmp_lt 0, 0
  .loc 1 6 5
  %t13 = scmp_ge 0, %t11
  .loc 1 6 5
  %t14 = zext1 %t12
  .loc 1 6 5
  %t15 = zext1 %t13
  .loc 1 6 5
  %t16 = or %t14, %t15
  .loc 1 6 5
  %t17 = icmp_ne %t16, 0
  .loc 1 6 5
  cbr %t17, bc_oob0, bc_ok0
UL999999994:
  .loc 1 7 7
  %t18 = load str, %message
  .loc 1 7 7
  call @rt_str_retain_maybe(%t18)
  .loc 1 7 7
  call @Zanna.Terminal.PrintStr(%t18)
  .loc 1 7 7
  call @rt_str_release_maybe(%t18)
  .loc 1 7 7
  %t19 = const_str @.L1
  .loc 1 7 7
  call @Zanna.Terminal.PrintStr(%t19)
  .loc 1 7 1
  br UL999999993
UL999999993:
  .loc 1 8 7
  %t20 = load i64, %count
  .loc 1 8 7
  call @Zanna.Terminal.PrintI64(%t20)
  .loc 1 8 7
  %t21 = const_str @.L1
  .loc 1 8 7
  call @Zanna.Terminal.PrintStr(%t21)
  .loc 1 8 1
  br UL999999992
UL999999992:
  .loc 1 9 7
  %t22 = load ptr, %values
  .loc 1 9 7
  %t23:i64 = call @rt_arr_i64_len(%t22)
  .loc 1 9 7
  %t24 = scmp_lt 0, 0
  .loc 1 9 7
  %t25 = scmp_ge 0, %t23
  .loc 1 9 7
  %t26 = zext1 %t24
  .loc 1 9 7
  %t27 = zext1 %t25
  .loc 1 9 7
  %t28 = or %t26, %t27
  .loc 1 9 7
  %t29 = icmp_ne %t28, 0
  .loc 1 9 7
  cbr %t29, bc_oob1, bc_ok1
exit:
  %t32 = load ptr, %values
  call @rt_arr_i64_release(%t32)
  store ptr, %values, null
  ret 0
bc_ok0:
  .loc 1 6 5
  call @rt_arr_i64_set(%t10, 0, %t9)
  .loc 1 6 1
  br UL999999994
bc_oob0:
  .loc 1 6 5
  call @rt_arr_oob_panic(0, %t11)
  .loc 1 6 5
  trap
bc_ok1:
  .loc 1 9 7
  %t30:i64 = call @rt_arr_i64_get(%t22, 0)
  .loc 1 9 7
  call @Zanna.Terminal.PrintI64(%t30)
  .loc 1 9 7
  %t31 = const_str @.L1
  .loc 1 9 7
  call @Zanna.Terminal.PrintStr(%t31)
  .loc 1 9 1
  br exit
bc_oob1:
  .loc 1 9 7
  call @rt_arr_oob_panic(0, %t23)
  .loc 1 9 7
  trap
}
