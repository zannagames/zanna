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
extern @rt_str_index_of(str, str) -> i64
extern @rt_str_instr3(i64, str, str) -> i64
extern @rt_str_empty() -> str
extern @rt_str_retain_maybe(str) -> void
extern @rt_str_release_maybe(str) -> void
global const str @.L0 = "HELLO"
global const str @.L1 = "LL"
global const str @.L2 = "\n"
global const str @.L3 = "L"
func @__mod_init$oop() -> void {
entry:
  ret
}
func @main() -> i64 {
entry:
  call @__mod_init$oop()
  %a$ = alloca 8
  %t1:str = call @rt_str_empty()
  store str, %a$, %t1
  br L10
L10:
  .loc 1 1 13
  %t2 = const_str @.L0
  .loc 1 1 4
  call @rt_str_retain_maybe(%t2)
  .loc 1 1 4
  %t3 = load str, %a$
  .loc 1 1 4
  call @rt_str_release_maybe(%t3)
  .loc 1 1 4
  store str, %a$, %t2
  .loc 1 1 4
  br L20
L20:
  .loc 1 2 16
  %t4 = load str, %a$
  .loc 1 2 20
  %t5 = const_str @.L1
  .loc 1 2 20
  %t6:i64 = call @rt_str_index_of(%t4, %t5)
  .loc 1 2 20
  call @Zanna.Terminal.PrintI64(%t6)
  .loc 1 2 20
  %t7 = const_str @.L2
  .loc 1 2 20
  call @Zanna.Terminal.PrintStr(%t7)
  .loc 1 2 4
  br L30
L30:
  .loc 1 3 16
  %t8 = iadd.ovf 3, -1
  .loc 1 3 19
  %t9 = load str, %a$
  .loc 1 3 23
  %t10 = const_str @.L3
  .loc 1 3 23
  %t11:i64 = call @rt_str_instr3(%t8, %t9, %t10)
  .loc 1 3 23
  call @Zanna.Terminal.PrintI64(%t11)
  .loc 1 3 23
  %t12 = const_str @.L2
  .loc 1 3 23
  call @Zanna.Terminal.PrintStr(%t12)
  .loc 1 3 4
  br L40
L40:
  .loc 1 4 4
  ret 0
exit:
  ret 0
}
