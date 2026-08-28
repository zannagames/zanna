REM Demonstrate ADDFILE inside a namespace
10 NAMESPACE Demo.App
20   ADDFILE "addfile_ns_inc.bas"
30 END NAMESPACE
40 PRINT "Back in ns main"
50 Demo.App.NsProc()
60 END
