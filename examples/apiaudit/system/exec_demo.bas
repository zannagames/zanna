' =============================================================================
' API Audit: Zanna.System.Exec - Process Execution
' =============================================================================
' Tests: Run, Capture, Shell, ShellCapture, ShellResult
' NOTE: Use safe commands only
' =============================================================================

PRINT "=== API Audit: Zanna.System.Exec ==="

' --- Run ---
PRINT "--- Run ---"
DIM rc AS INTEGER
rc = Zanna.System.Exec.Run("echo hello")
PRINT "Run('echo hello') exit code: "; rc

' --- Capture ---
PRINT "--- Capture ---"
DIM captured AS STRING
captured = Zanna.System.Exec.Capture("echo hello")
PRINT "Capture('echo hello'): "; captured

' --- Shell ---
PRINT "--- Shell ---"
DIM sc AS INTEGER
sc = Zanna.System.Exec.Shell("echo shell_test")
PRINT "Shell('echo shell_test') exit code: "; sc

' --- ShellCapture ---
PRINT "--- ShellCapture ---"
DIM sout AS STRING
sout = Zanna.System.Exec.ShellCapture("echo captured_output")
PRINT "ShellCapture('echo captured_output'): "; sout

' --- ShellResult ---
PRINT "--- ShellResult ---"
DIM shellResult AS OBJECT
shellResult = Zanna.System.Exec.ShellResult("echo result_output")
PRINT "ShellResult Output: "; Zanna.System.CommandResult.get_Output(shellResult)
PRINT "ShellResult ExitCode: "; Zanna.System.CommandResult.get_ExitCode(shellResult)
PRINT "ShellResult Succeeded: "; Zanna.System.CommandResult.get_IsSuccess(shellResult)

' Test with shell features
PRINT "--- ShellCapture with pipe ---"
DIM piped AS STRING
piped = Zanna.System.Exec.ShellCapture("echo abc | tr a-z A-Z")
PRINT "ShellCapture('echo abc | tr a-z A-Z'): "; piped

PRINT "=== Exec Demo Complete ==="
END
