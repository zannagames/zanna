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
  %t1 = alloca 8
  br L10
L10:
  .loc 1 1 4
  %t2:ptr = call @rt_arr_i64_new(3)
  .loc 1 1 4
  call @rt_arr_i64_retain(%t2)
  .loc 1 1 4
  %t3 = load ptr, %a
  .loc 1 1 4
  call @rt_arr_i64_release(%t3)
  .loc 1 1 4
  store ptr, %a, %t2
  .loc 1 1 4
  store i64, %t1, 3
  .loc 1 1 4
  br L20
L20:
  .loc 1 2 10
  %t4 = load ptr, %a
  .loc 1 2 10
  %t5:i64 = call @rt_arr_i64_len(%t4)
  .loc 1 2 10
  %t6 = scmp_lt 1, 0
  .loc 1 2 10
  %t7 = scmp_ge 1, %t5
  .loc 1 2 10
  %t8 = zext1 %t6
  .loc 1 2 10
  %t9 = zext1 %t7
  .loc 1 2 10
  %t10 = or %t8, %t9
  .loc 1 2 10
  %t11 = icmp_ne %t10, 0
  .loc 1 2 10
  cbr %t11, bc_oob0, bc_ok0
exit:
  %t14 = load ptr, %a
  call @rt_arr_i64_release(%t14)
  store ptr, %a, null
  ret 0
bc_ok0:
  .loc 1 2 10
  %t12:i64 = call @rt_arr_i64_get(%t4, 1)
  .loc 1 2 10
  call @Zanna.Terminal.PrintI64(%t12)
  .loc 1 2 10
  %t13 = const_str @.L0
  .loc 1 2 10
  call @Zanna.Terminal.PrintStr(%t13)
  .loc 1 2 4
  br exit
bc_oob0:
  .loc 1 2 10
  call @rt_arr_oob_panic(1, %t5)
  .loc 1 2 10
  trap
}
