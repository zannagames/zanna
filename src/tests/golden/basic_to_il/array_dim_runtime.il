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
extern @rt_arr_i64_resize(ptr, i64) -> ptr
extern @rt_arr_i64_len(ptr) -> i64
extern @rt_arr_i64_get(ptr, i64) -> i64
extern @rt_arr_i64_set(ptr, i64, i64) -> void
extern @rt_arr_i64_retain(ptr) -> void
extern @rt_arr_i64_release(ptr) -> void
extern @rt_arr_oob_panic(i64, i64) -> void
global const str @.L0 = "\n"
func @__mod_init$oop() -> void {
entry:
  ret
}
func @main() -> i64 {
entry:
  call @__mod_init$oop()
  %a = alloca 8
  store ptr, %a, null
  %ub = alloca 8
  %x = alloca 8
  %t3 = alloca 8
  br L10
L10:
  .loc 1 1 4
  %t4:ptr = call @rt_arr_i64_new(2)
  .loc 1 1 4
  call @rt_arr_i64_retain(%t4)
  .loc 1 1 4
  %t5 = load ptr, %a
  .loc 1 1 4
  call @rt_arr_i64_release(%t5)
  .loc 1 1 4
  store ptr, %a, %t4
  .loc 1 1 4
  store i64, %t3, 2
  .loc 1 1 4
  br L20
L20:
  .loc 1 2 4
  store i64, %ub, 1
  .loc 1 2 4
  br L30
L30:
  .loc 1 3 12
  %t6 = load i64, %ub
  .loc 1 3 15
  %t7 = iadd.ovf %t6, 2
  .loc 1 3 4
  %t8 = iadd.ovf %t7, 1
  .loc 1 3 4
  %t10 = scmp_lt %t8, 0
  .loc 1 3 4
  cbr %t10, redim_len_fail, redim_len_cont(%t8)
L40:
  .loc 1 4 4
  %t14 = load ptr, %a
  .loc 1 4 10
  %t15 = load i64, %ub
  .loc 1 4 8
  %t16:i64 = call @rt_arr_i64_len(%t14)
  .loc 1 4 8
  %t17 = scmp_lt %t15, 0
  .loc 1 4 8
  %t18 = scmp_ge %t15, %t16
  .loc 1 4 8
  %t19 = zext1 %t17
  .loc 1 4 8
  %t20 = zext1 %t18
  .loc 1 4 8
  %t21 = or %t19, %t20
  .loc 1 4 8
  %t22 = icmp_ne %t21, 0
  .loc 1 4 8
  cbr %t22, bc_oob0, bc_ok0
L50:
  .loc 1 5 12
  %t23 = load ptr, %a
  .loc 1 5 14
  %t24 = load i64, %ub
  .loc 1 5 12
  %t25:i64 = call @rt_arr_i64_len(%t23)
  .loc 1 5 12
  %t26 = scmp_lt %t24, 0
  .loc 1 5 12
  %t27 = scmp_ge %t24, %t25
  .loc 1 5 12
  %t28 = zext1 %t26
  .loc 1 5 12
  %t29 = zext1 %t27
  .loc 1 5 12
  %t30 = or %t28, %t29
  .loc 1 5 12
  %t31 = icmp_ne %t30, 0
  .loc 1 5 12
  cbr %t31, bc_oob1, bc_ok1
L60:
  .loc 1 6 10
  %t33 = load i64, %ub
  .loc 1 6 10
  call @Zanna.Terminal.PrintI64(%t33)
  .loc 1 6 10
  %t34 = const_str @.L0
  .loc 1 6 10
  call @Zanna.Terminal.PrintStr(%t34)
  .loc 1 6 4
  br L70
L70:
  .loc 1 7 10
  %t35 = load i64, %x
  .loc 1 7 10
  call @Zanna.Terminal.PrintI64(%t35)
  .loc 1 7 10
  %t36 = const_str @.L0
  .loc 1 7 10
  call @Zanna.Terminal.PrintStr(%t36)
  .loc 1 7 4
  br L80
L80:
  .loc 1 8 4
  ret 0
exit:
  %t37 = load ptr, %a
  call @rt_arr_i64_release(%t37)
  store ptr, %a, null
  ret 0
redim_len_fail:
  .loc 1 3 4
  trap
redim_len_cont(%len:i64):
  .loc 1 3 15
  %t11 = load ptr, %a
  .loc 1 3 15
  %t12:ptr = call @rt_arr_i64_resize(%t11, %len)
  .loc 1 3 15
  call @rt_arr_i64_retain(%t12)
  .loc 1 3 15
  %t13 = load ptr, %a
  .loc 1 3 15
  call @rt_arr_i64_release(%t13)
  .loc 1 3 15
  store ptr, %a, %t12
  .loc 1 3 15
  store i64, %t3, %len
  .loc 1 3 4
  br L40
bc_ok0:
  .loc 1 4 8
  call @rt_arr_i64_set(%t14, %t15, 7)
  .loc 1 4 4
  br L50
bc_oob0:
  .loc 1 4 8
  call @rt_arr_oob_panic(%t15, %t16)
  .loc 1 4 8
  trap
bc_ok1:
  .loc 1 5 12
  %t32:i64 = call @rt_arr_i64_get(%t23, %t24)
  .loc 1 5 4
  store i64, %x, %t32
  .loc 1 5 4
  br L60
bc_oob1:
  .loc 1 5 12
  call @rt_arr_oob_panic(%t24, %t25)
  .loc 1 5 12
  trap
}
