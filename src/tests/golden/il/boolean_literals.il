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
extern @rt_str_release_maybe(str) -> void
global const str @.L0 = "\n"
func @__mod_init$oop() -> void {
entry:
  ret
}
func @main() -> i64 {
entry:
  call @__mod_init$oop()
  %b = alloca 1
  %t1 = trunc1 0
  store i1, %b, %t1
  %a = alloca 8
  br L10
L10:
  .loc 1 1 4
  %t3:str = call @Zanna.Terminal.ReadLine()
  .loc 1 1 4
  %t4:i64 = call @Zanna.Core.Convert.ToInt64(%t3)
  .loc 1 1 4
  store i64, %a, %t4
  .loc 1 1 4
  call @rt_str_release_maybe(%t3)
  .loc 1 1 4
  br L20
L20:
  .loc 1 2 4
  br L30
L30:
  .loc 1 3 12
  %t5 = load i64, %a
  .loc 1 3 14
  %t6 = icmp_ne %t5, 0
  .loc 1 3 14
  %t7 = zext1 %t6
  .loc 1 3 14
  %t8 = isub.ovf 0, %t7
  .loc 1 3 4
  %t9 = trunc1 %t8
  .loc 1 3 4
  store i1, %b, %t9
  .loc 1 3 4
  br L40
L40:
  .loc 1 4 10
  %t10 = load i1, %b
  .loc 1 4 12
  %t11 = zext1 %t10
  .loc 1 4 12
  %t12 = isub.ovf 0, %t11
  .loc 1 4 12
  %t13 = or %t12, -1
  .loc 1 4 12
  call @Zanna.Terminal.PrintI64(%t13)
  .loc 1 4 12
  %t14 = const_str @.L0
  .loc 1 4 12
  call @Zanna.Terminal.PrintStr(%t14)
  .loc 1 4 4
  br L50
L50:
  .loc 1 5 20
  %t15 = load i1, %b
  .loc 1 5 16
  %t16 = zext1 %t15
  .loc 1 5 16
  %t17 = isub.ovf 0, %t16
  .loc 1 5 16
  %t18 = and 0, %t17
  .loc 1 5 16
  call @Zanna.Terminal.PrintI64(%t18)
  .loc 1 5 16
  %t19 = const_str @.L0
  .loc 1 5 16
  call @Zanna.Terminal.PrintStr(%t19)
  .loc 1 5 4
  br exit
exit:
  ret 0
}
