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
extern @rt_trap_string(str) -> void
func @__mod_init$oop() -> void {
entry:
  ret
}
func @F() -> i64 {
entry_F:
  %i = alloca 8
  br L-1000000000_F
L-1000000000_F:
  .loc 1 2 1
  br while_head_0_F
L-999999999_F:
  .loc 1 4 14
  store i64, %i, 1
  .loc 1 4 1
  %t2 = scmp_ge 1, 0
  .loc 1 4 1
  cbr %t2, for_head_pos_0_F, for_head_neg_0_F
L-999999998_F:
  .loc 1 6 8
  ret 0
ret_F:
  ret 0
while_head_0_F:
  .loc 1 2 1
  %t1 = trunc1 0
  .loc 1 2 7
  cbr %t1, while_body_0_F, while_end_0_F
while_body_0_F:
  .loc 1 2 7
  br while_head_0_F
while_end_0_F:
  .loc 1 2 1
  br L-999999999_F
for_head_pos_0_F:
  .loc 1 4 1
  %t3 = load i64, %i
  .loc 1 4 1
  %t4 = scmp_le %t3, 1
  .loc 1 4 1
  cbr %t4, for_body_1_F, for_end_1_F
for_head_neg_0_F:
  .loc 1 4 1
  %t5 = load i64, %i
  .loc 1 4 1
  %t6 = scmp_ge %t5, 1
  .loc 1 4 1
  cbr %t6, for_body_1_F, for_end_1_F
for_body_1_F:
  .loc 1 4 1
  br for_inc_1_F
for_inc_1_F:
  .loc 1 4 1
  %t7 = load i64, %i
  .loc 1 4 1
  %t8 = iadd.ovf %t7, 1
  .loc 1 4 1
  store i64, %i, %t8
  .loc 1 4 1
  cbr %t2, for_head_pos_0_F, for_head_neg_0_F
for_end_1_F:
  .loc 1 4 1
  br L-999999998_F
}
func @main() -> i64 {
entry:
  call @__mod_init$oop()
  ret 0
exit:
  ret 0
}
