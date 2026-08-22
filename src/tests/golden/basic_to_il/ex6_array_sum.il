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
extern @Zanna.Core.Convert.ToInt64(str) -> i64
extern @Zanna.Terminal.ReadLine() -> str
extern @rt_arr_i64_new(i64) -> ptr
extern @rt_arr_i64_len(ptr) -> i64
extern @rt_arr_i64_get(ptr, i64) -> i64
extern @rt_arr_i64_set(ptr, i64, i64) -> void
extern @rt_arr_i64_retain(ptr) -> void
extern @rt_arr_i64_release(ptr) -> void
extern @rt_arr_oob_panic(i64, i64) -> void
extern @rt_str_release_maybe(str) -> void
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
  %i = alloca 8
  %n = alloca 8
  %s = alloca 8
  %t4 = alloca 8
  br L10
L10:
  .loc 1 1 4
  %t5:str = call @Zanna.Terminal.ReadLine()
  .loc 1 1 4
  %t6:i64 = call @Zanna.Core.Convert.ToInt64(%t5)
  .loc 1 1 4
  store i64, %n, %t6
  .loc 1 1 4
  call @rt_str_release_maybe(%t5)
  .loc 1 1 4
  br L20
L20:
  .loc 1 2 10
  %t7 = load i64, %n
  .loc 1 2 4
  %t8 = iadd.ovf %t7, 1
  .loc 1 2 4
  %t10 = scmp_lt %t8, 0
  .loc 1 2 4
  cbr %t10, dim_len_fail, dim_len_cont(%t8)
L30:
  .loc 1 3 4
  store i64, %i, 0
  .loc 1 3 4
  br L40
L40:
  .loc 1 4 4
  store i64, %s, 0
  .loc 1 4 4
  br L50
L50:
  .loc 1 5 4
  br loop_head
L100:
  .loc 1 10 11
  %t45 = load i64, %s
  .loc 1 10 11
  call @Zanna.Terminal.PrintI64(%t45)
  .loc 1 10 11
  %t46 = const_str @.L0
  .loc 1 10 11
  call @Zanna.Terminal.PrintStr(%t46)
  .loc 1 10 5
  br L110
L110:
  .loc 1 11 5
  ret 0
exit:
  %t47 = load ptr, %a
  call @rt_arr_i64_release(%t47)
  store ptr, %a, null
  ret 0
dim_len_fail:
  .loc 1 2 4
  trap
dim_len_cont(%len:i64):
  .loc 1 2 10
  %t11:ptr = call @rt_arr_i64_new(%len)
  .loc 1 2 10
  call @rt_arr_i64_retain(%t11)
  .loc 1 2 10
  %t12 = load ptr, %a
  .loc 1 2 10
  call @rt_arr_i64_release(%t12)
  .loc 1 2 10
  store ptr, %a, %t11
  .loc 1 2 10
  store i64, %t4, %len
  .loc 1 2 4
  br L30
loop_head:
  .loc 1 5 10
  %t13 = load i64, %i
  .loc 1 5 14
  %t14 = load i64, %n
  .loc 1 5 12
  %t15 = scmp_lt %t13, %t14
  .loc 1 5 12
  %t16 = zext1 %t15
  .loc 1 5 12
  %t17 = isub.ovf 0, %t16
  .loc 1 5 4
  %t18 = trunc1 %t17
  .loc 1 5 12
  cbr %t18, loop_body, done
loop_body:
  .loc 1 6 17
  %t19 = load i64, %i
  .loc 1 6 21
  %t20 = load i64, %i
  .loc 1 6 19
  %t21 = imul.ovf %t19, %t20
  .loc 1 6 6
  %t22 = load ptr, %a
  .loc 1 6 12
  %t23 = load i64, %i
  .loc 1 6 10
  %t24:i64 = call @rt_arr_i64_len(%t22)
  .loc 1 6 10
  %t25 = scmp_lt %t23, 0
  .loc 1 6 10
  %t26 = scmp_ge %t23, %t24
  .loc 1 6 10
  %t27 = zext1 %t25
  .loc 1 6 10
  %t28 = zext1 %t26
  .loc 1 6 10
  %t29 = or %t27, %t28
  .loc 1 6 10
  %t30 = icmp_ne %t29, 0
  .loc 1 6 10
  cbr %t30, bc_oob0, bc_ok0
done:
  .loc 1 5 4
  br L100
bc_ok0:
  .loc 1 6 10
  call @rt_arr_i64_set(%t22, %t23, %t21)
  .loc 1 7 14
  %t31 = load i64, %s
  .loc 1 7 18
  %t32 = load ptr, %a
  .loc 1 7 20
  %t33 = load i64, %i
  .loc 1 7 18
  %t34:i64 = call @rt_arr_i64_len(%t32)
  .loc 1 7 18
  %t35 = scmp_lt %t33, 0
  .loc 1 7 18
  %t36 = scmp_ge %t33, %t34
  .loc 1 7 18
  %t37 = zext1 %t35
  .loc 1 7 18
  %t38 = zext1 %t36
  .loc 1 7 18
  %t39 = or %t37, %t38
  .loc 1 7 18
  %t40 = icmp_ne %t39, 0
  .loc 1 7 18
  cbr %t40, bc_oob1, bc_ok1
bc_oob0:
  .loc 1 6 10
  call @rt_arr_oob_panic(%t23, %t24)
  .loc 1 6 10
  trap
bc_ok1:
  .loc 1 7 18
  %t41:i64 = call @rt_arr_i64_get(%t32, %t33)
  .loc 1 7 16
  %t42 = iadd.ovf %t31, %t41
  .loc 1 7 6
  store i64, %s, %t42
  .loc 1 8 14
  %t43 = load i64, %i
  .loc 1 8 16
  %t44 = iadd.ovf %t43, 1
  .loc 1 8 6
  store i64, %i, %t44
  .loc 1 8 6
  br loop_head
bc_oob1:
  .loc 1 7 18
  call @rt_arr_oob_panic(%t33, %t34)
  .loc 1 7 18
  trap
}
