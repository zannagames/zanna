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
global const str @.L0 = "\n"
func @__mod_init$oop() -> void {
entry:
  ret
}
func @main() -> i64 {
entry:
  call @__mod_init$oop()
  %i = alloca 8
  %j = alloca 8
  br L10
L10:
  .loc 1 1 17
  store i64, %i, 1
  .loc 1 1 4
  %t2 = scmp_ge 1, 0
  .loc 1 1 4
  cbr %t2, for_head_pos, for_head_neg
exit:
  ret 0
for_head_pos:
  .loc 1 1 4
  %t3 = load i64, %i
  .loc 1 1 4
  %t4 = scmp_le %t3, 5
  .loc 1 1 4
  cbr %t4, for_body, for_done
for_head_neg:
  .loc 1 1 4
  %t5 = load i64, %i
  .loc 1 1 4
  %t6 = scmp_ge %t5, 5
  .loc 1 1 4
  cbr %t6, for_body, for_done
for_body:
  .loc 1 2 17
  store i64, %j, 1
  .loc 1 2 4
  %t7 = scmp_ge 1, 0
  .loc 1 2 4
  cbr %t7, for_head_pos1, for_head_neg1
for_inc:
  .loc 1 2 4
  %t18 = load i64, %i
  .loc 1 2 4
  %t19 = iadd.ovf %t18, 1
  .loc 1 2 4
  store i64, %i, %t19
  .loc 1 2 4
  cbr %t2, for_head_pos, for_head_neg
for_done:
  .loc 1 1 4
  br exit
for_head_pos1:
  .loc 1 2 4
  %t8 = load i64, %j
  .loc 1 2 4
  %t9 = scmp_le %t8, 5
  .loc 1 2 4
  cbr %t9, for_body1, for_done1
for_head_neg1:
  .loc 1 2 4
  %t10 = load i64, %j
  .loc 1 2 4
  %t11 = scmp_ge %t10, 5
  .loc 1 2 4
  cbr %t11, for_body1, for_done1
for_body1:
  .loc 1 3 10
  %t12 = load i64, %i
  .loc 1 3 14
  %t13 = load i64, %j
  .loc 1 3 12
  %t14 = imul.ovf %t12, %t13
  .loc 1 3 12
  call @Zanna.Terminal.PrintI64(%t14)
  .loc 1 3 12
  %t15 = const_str @.L0
  .loc 1 3 12
  call @Zanna.Terminal.PrintStr(%t15)
  .loc 1 3 4
  br for_inc1
for_inc1:
  .loc 1 3 4
  %t16 = load i64, %j
  .loc 1 3 4
  %t17 = iadd.ovf %t16, 1
  .loc 1 3 4
  store i64, %j, %t17
  .loc 1 3 4
  cbr %t7, for_head_pos1, for_head_neg1
for_done1:
  .loc 1 2 4
  br for_inc
}
