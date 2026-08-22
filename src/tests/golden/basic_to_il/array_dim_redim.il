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
extern @rt_arr_i64_retain(ptr) -> void
extern @rt_arr_i64_release(ptr) -> void
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
  %t2:ptr = call @rt_arr_i64_new(4)
  .loc 1 1 4
  call @rt_arr_i64_retain(%t2)
  .loc 1 1 4
  %t3 = load ptr, %a
  .loc 1 1 4
  call @rt_arr_i64_release(%t3)
  .loc 1 1 4
  store ptr, %a, %t2
  .loc 1 1 4
  store i64, %t1, 4
  .loc 1 1 4
  br L20
L20:
  .loc 1 2 4
  %t4 = iadd.ovf 5, 1
  .loc 1 2 4
  %t6 = scmp_lt %t4, 0
  .loc 1 2 4
  cbr %t6, redim_len_fail, redim_len_cont(%t4)
L30:
  .loc 1 3 4
  ret 0
exit:
  %t10 = load ptr, %a
  call @rt_arr_i64_release(%t10)
  store ptr, %a, null
  ret 0
redim_len_fail:
  .loc 1 2 4
  trap
redim_len_cont(%len:i64):
  .loc 1 2 12
  %t7 = load ptr, %a
  .loc 1 2 12
  %t8:ptr = call @rt_arr_i64_resize(%t7, %len)
  .loc 1 2 12
  call @rt_arr_i64_retain(%t8)
  .loc 1 2 12
  %t9 = load ptr, %a
  .loc 1 2 12
  call @rt_arr_i64_release(%t9)
  .loc 1 2 12
  store ptr, %a, %t8
  .loc 1 2 12
  store i64, %t1, %len
  .loc 1 2 4
  br L30
}
