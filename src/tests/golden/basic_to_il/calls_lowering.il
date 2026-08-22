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
global const str @.L0 = ""
global const str @.L1 = "\n"
global const str @.L2 = "HI"
func @__mod_init$oop() -> void {
entry:
  ret
}
func @F#(f64 %X#) -> f64 {
entry_F#(%X#:f64):
  %t1 = alloca 8
  store f64, %t1, %X#
  br L-1000000000_F#
L-1000000000_F#:
  .loc 1 2 8
  %t2 = load f64, %t1
  .loc 1 2 8
  ret %t2
ret_F#:
  ret 0.0
}
func @ID$(str %S$) -> str {
entry_ID$(%S$:str):
  %t1 = alloca 8
  store str, %t1, %S$
  br L-1000000000_ID$
L-1000000000_ID$:
  .loc 1 5 8
  %t2 = load str, %t1
  .loc 1 5 8
  ret %t2
ret_ID$:
  %t3 = const_str @.L0
  ret %t3
}
func @FIRST#(i64 %N) -> f64 {
entry_FIRST#(%N:i64):
  %t1 = alloca 8
  store i64, %t1, %N
  br L-1000000000_FIRST#
L-1000000000_FIRST#:
  .loc 1 8 1
  br if_test_0_FIRST#
L-999999999_FIRST#:
  .loc 1 9 16
  %t7 = load i64, %t1
  .loc 1 9 8
  %t8 = sitofp %t7
  .loc 1 9 8
  %t9:f64 = call @SECOND#(%t8)
  .loc 1 9 8
  ret %t9
ret_FIRST#:
  ret 0.0
if_test_0_FIRST#:
  .loc 1 8 4
  %t2 = load i64, %t1
  .loc 1 8 6
  %t3 = scmp_le %t2, 0
  .loc 1 8 6
  %t4 = zext1 %t3
  .loc 1 8 6
  %t5 = isub.ovf 0, %t4
  .loc 1 8 1
  %t6 = trunc1 %t5
  .loc 1 8 6
  cbr %t6, if_then_0_FIRST#, if_else_0_FIRST#
if_then_0_FIRST#:
  .loc 1 8 23
  ret 0.0
if_else_0_FIRST#:
  .loc 1 8 1
  br if_end_0_FIRST#
if_end_0_FIRST#:
  .loc 1 8 1
  br L-999999999_FIRST#
}
func @SECOND#(f64 %N#) -> f64 {
entry_SECOND#(%N#:f64):
  %t1 = alloca 8
  store f64, %t1, %N#
  br L-1000000000_SECOND#
L-1000000000_SECOND#:
  .loc 1 12 1
  br if_test_0_SECOND#
L-999999999_SECOND#:
  .loc 1 13 15
  %t8 = load f64, %t1
  .loc 1 13 18
  %t9 = sitofp 1
  .loc 1 13 18
  %t10 = fsub %t8, %t9
  .loc 1 13 8
  %t11 = cast.fp_to_si.rte.chk %t10
  .loc 1 13 8
  %t12:f64 = call @FIRST#(%t11)
  .loc 1 13 8
  ret %t12
ret_SECOND#:
  ret 0.0
if_test_0_SECOND#:
  .loc 1 12 4
  %t2 = load f64, %t1
  .loc 1 12 7
  %t3 = sitofp 0
  .loc 1 12 7
  %t4 = fcmp_le %t2, %t3
  .loc 1 12 7
  %t5 = zext1 %t4
  .loc 1 12 7
  %t6 = isub.ovf 0, %t5
  .loc 1 12 1
  %t7 = trunc1 %t6
  .loc 1 12 7
  cbr %t7, if_then_0_SECOND#, if_else_0_SECOND#
if_then_0_SECOND#:
  .loc 1 12 24
  ret 0.0
if_else_0_SECOND#:
  .loc 1 12 1
  br if_end_0_SECOND#
if_end_0_SECOND#:
  .loc 1 12 1
  br L-999999999_SECOND#
}
func @main() -> i64 {
entry:
  call @__mod_init$oop()
  br L10
L10:
  .loc 1 15 10
  %t0 = sitofp 1
  .loc 1 15 10
  %t1:f64 = call @F#(%t0)
  .loc 1 15 10
  call @Zanna.Terminal.PrintF64(%t1)
  .loc 1 15 10
  %t2 = const_str @.L1
  .loc 1 15 10
  call @Zanna.Terminal.PrintStr(%t2)
  .loc 1 15 4
  br L20
L20:
  .loc 1 16 14
  %t3 = const_str @.L2
  .loc 1 16 10
  %t4:str = call @ID$(%t3)
  .loc 1 16 10
  call @Zanna.Terminal.PrintStr(%t4)
  .loc 1 16 10
  %t5 = const_str @.L1
  .loc 1 16 10
  call @Zanna.Terminal.PrintStr(%t5)
  .loc 1 16 4
  br L30
L30:
  .loc 1 17 10
  %t6:f64 = call @FIRST#(2)
  .loc 1 17 10
  call @Zanna.Terminal.PrintF64(%t6)
  .loc 1 17 10
  %t7 = const_str @.L1
  .loc 1 17 10
  call @Zanna.Terminal.PrintStr(%t7)
  .loc 1 17 4
  br L40
L40:
  .loc 1 18 4
  ret 0
exit:
  ret 0
}
