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
global const str @.L0 = "HELLO"
global const str @.L1 = "\n"
global const str @.L2 = "READY"
func @__mod_init$oop() -> void {
entry:
  ret
}
func @main() -> i64 {
entry:
  call @__mod_init$oop()
  %x = alloca 8
  %y = alloca 8
  br L10
L10:
  .loc 1 1 4
  store i64, %x, 5
  .loc 1 1 4
  br L20
L20:
  .loc 1 2 12
  %t2 = load i64, %x
  .loc 1 2 14
  %t3 = imul.ovf %t2, 2
  .loc 1 2 4
  store i64, %y, %t3
  .loc 1 2 4
  br L30
L30:
  .loc 1 3 10
  %t4 = const_str @.L0
  .loc 1 3 10
  call @Zanna.Terminal.PrintStr(%t4)
  .loc 1 3 10
  %t5 = const_str @.L1
  .loc 1 3 10
  call @Zanna.Terminal.PrintStr(%t5)
  .loc 1 3 4
  br L40
L40:
  .loc 1 4 10
  %t6 = const_str @.L2
  .loc 1 4 10
  call @Zanna.Terminal.PrintStr(%t6)
  .loc 1 4 10
  %t7 = const_str @.L1
  .loc 1 4 10
  call @Zanna.Terminal.PrintStr(%t7)
  .loc 1 4 4
  br L50
L50:
  .loc 1 5 10
  %t8 = load i64, %y
  .loc 1 5 10
  call @Zanna.Terminal.PrintI64(%t8)
  .loc 1 5 10
  %t9 = const_str @.L1
  .loc 1 5 10
  call @Zanna.Terminal.PrintStr(%t9)
  .loc 1 5 4
  br L60
L60:
  .loc 1 6 4
  br if_test_0
L70:
  .loc 1 7 4
  ret 0
exit:
  ret 0
if_test_0:
  .loc 1 6 7
  %t10 = load i64, %y
  .loc 1 6 9
  %t11 = scmp_gt %t10, 8
  .loc 1 6 9
  %t12 = zext1 %t11
  .loc 1 6 9
  %t13 = isub.ovf 0, %t12
  .loc 1 6 4
  %t14 = trunc1 %t13
  .loc 1 6 9
  cbr %t14, if_then_0, if_else
if_then_0:
  .loc 1 6 24
  %t15 = load i64, %y
  .loc 1 6 24
  call @Zanna.Terminal.PrintI64(%t15)
  .loc 1 6 24
  %t16 = const_str @.L1
  .loc 1 6 24
  call @Zanna.Terminal.PrintStr(%t16)
  .loc 1 6 4
  br if_exit
if_else:
  .loc 1 6 37
  call @Zanna.Terminal.PrintI64(4)
  .loc 1 6 37
  %t17 = const_str @.L1
  .loc 1 6 37
  call @Zanna.Terminal.PrintStr(%t17)
  .loc 1 6 4
  br if_exit
if_exit:
  .loc 1 6 4
  br L70
}
