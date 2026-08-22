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
extern @rt_open_err_vstr(str, i32, i32) -> i32
extern @rt_close_err(i32) -> i32
global const str @.L0 = "foo.txt"
func @__mod_init$oop() -> void {
entry:
  ret
}
func @main() -> i64 {
entry:
  call @__mod_init$oop()
  br L10
L10:
  .loc 1 1 9
  %t0 = const_str @.L0
  .loc 1 1 4
  %t1:i32 = cast.si_narrow.chk 1
  .loc 1 1 4
  %t2:i32 = cast.si_narrow.chk 1
  .loc 1 1 4
  %t3:i32 = call @rt_open_err_vstr(%t0, %t1, %t2)
  .loc 1 1 4
  %t4 = alloca 8
  .loc 1 1 4
  store i64, %t4, 0
  .loc 1 1 4
  store i32, %t4, %t3
  .loc 1 1 4
  %t5 = load i64, %t4
  .loc 1 1 4
  %t6 = icmp_ne %t5, 0
  .loc 1 1 4
  cbr %t6, open_fail, open_cont
L20:
  .loc 1 2 4
  %t7:i32 = cast.si_narrow.chk 1
  .loc 1 2 4
  %t8:i32 = call @rt_close_err(%t7)
  .loc 1 2 4
  %t9 = alloca 8
  .loc 1 2 4
  store i64, %t9, 0
  .loc 1 2 4
  store i32, %t9, %t8
  .loc 1 2 4
  %t10 = load i64, %t9
  .loc 1 2 4
  %t11 = icmp_ne %t10, 0
  .loc 1 2 4
  cbr %t11, close_fail, close_cont
exit:
  ret 0
open_fail:
  .loc 1 1 4
  trap.from_err i32 %t3
open_cont:
  .loc 1 1 4
  br L20
close_fail:
  .loc 1 2 4
  trap.from_err i32 %t8
close_cont:
  .loc 1 2 4
  br exit
}
