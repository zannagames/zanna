' =============================================================================
' API Audit: Zanna.System.Machine - System Information
' =============================================================================
' Tests: get_Cores, get_Endian, get_Home, get_Host, get_MemFree, get_MemTotal,
'        get_Os, get_OsVer, get_Temp, get_User
' =============================================================================

PRINT "=== API Audit: Zanna.System.Machine ==="

' --- get_Os ---
PRINT "--- get_Os ---"
PRINT "OS: "; Zanna.System.Machine.get_Os()

' --- get_OsVer ---
PRINT "--- get_OsVer ---"
PRINT "OSVer: "; Zanna.System.Machine.get_OsVersion()

' --- get_Cores ---
PRINT "--- get_Cores ---"
PRINT "Cores: "; Zanna.System.Machine.get_Cores()

' --- get_MemTotal ---
PRINT "--- get_MemTotal ---"
PRINT "MemTotal (bytes): "; Zanna.System.Machine.get_MemoryTotal()

' --- get_MemFree ---
PRINT "--- get_MemFree ---"
PRINT "MemFree (bytes): "; Zanna.System.Machine.get_MemoryFree()

' --- get_Endian ---
PRINT "--- get_Endian ---"
PRINT "Endian: "; Zanna.System.Machine.get_Endian()

' --- get_Host ---
PRINT "--- get_Host ---"
PRINT "Host: "; Zanna.System.Machine.get_Host()

' --- get_User ---
PRINT "--- get_User ---"
PRINT "User: "; Zanna.System.Machine.get_User()

' --- get_Home ---
PRINT "--- get_Home ---"
PRINT "Home: "; Zanna.System.Machine.get_Home()

' --- get_Temp ---
PRINT "--- get_Temp ---"
PRINT "Temp: "; Zanna.System.Machine.get_TempDir()

PRINT "=== Machine Demo Complete ==="
END
