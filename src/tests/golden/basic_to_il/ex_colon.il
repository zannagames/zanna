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
  %a = alloca 8
  %t1 = alloca 8
  %t2 = alloca 512
  store i64, %t1, 0
  br L10
L10:
  .loc 1 1 4
  store i64, %a, 1
  .loc 1 1 22
  %t3 = load i64, %a
  .loc 1 1 22
  call @Zanna.Terminal.PrintI64(%t3)
  .loc 1 1 22
  %t4 = const_str @.L0
  .loc 1 1 22
  call @Zanna.Terminal.PrintStr(%t4)
  .loc 1 1 34
  %t5 = load i64, %a
  .loc 1 1 36
  %t6 = iadd.ovf %t5, 1
  .loc 1 1 26
  store i64, %a, %t6
  .loc 1 1 48
  %t7 = load i64, %a
  .loc 1 1 48
  call @Zanna.Terminal.PrintI64(%t7)
  .loc 1 1 48
  %t8 = const_str @.L0
  .loc 1 1 48
  call @Zanna.Terminal.PrintStr(%t8)
  .loc 1 1 4
  br exit
exit:
  ret 0
}
