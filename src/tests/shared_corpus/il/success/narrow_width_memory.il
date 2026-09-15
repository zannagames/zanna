il 0.3.0
extern @Zanna.Terminal.PrintI64(i64) -> void
extern @Zanna.Terminal.PrintStr(str) -> void
global const str @.nl = "\n"

func @show(i64 %value) -> void {
entry(%value:i64):
  call @Zanna.Terminal.PrintI64(%value)
  %nl = const_str @.nl
  call @Zanna.Terminal.PrintStr(%nl)
  ret
}

func @main() -> i64 {
entry:
  %slot = alloca 32
  %hi = gep %slot, 8
  store i64, %slot, 0
  store i64, %hi, 1234605616436508552
  %b7 = gep %slot, 7
  %one = trunc1 1
  store i1, %b7, %one
  %after_i1 = load i64, %hi
  call @show(%after_i1)
  %w6 = gep %slot, 6
  %word:i16 = cast.si_narrow.chk 4660
  store i16, %w6, %word
  %after_i16 = load i64, %hi
  call @show(%after_i16)
  %d4 = gep %slot, 4
  %dword:i32 = cast.si_narrow.chk 305419896
  store i32, %d4, %dword
  %after_i32 = load i64, %hi
  call @show(%after_i32)
  %flags = gep %slot, 16
  store i64, %flags, 2
  %bit = load i1, %flags
  %bit64 = zext1 %bit
  call @show(%bit64)
  %copy = gep %slot, 24
  %low32 = load i32, %hi
  store i32, %copy, %low32
  %copied32 = load i64, %copy
  call @show(%copied32)
  store i64, %copy, 0
  %low16 = load i16, %hi
  store i16, %copy, %low16
  %copied16 = load i64, %copy
  call @show(%copied16)
  ret 0
}
