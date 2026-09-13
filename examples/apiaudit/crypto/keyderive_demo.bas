' =============================================================================
' API Audit: Zanna.Crypto.KeyDerive (BASIC)
' =============================================================================
' Tests: Pbkdf2SHA256, Pbkdf2SHA256Str
' =============================================================================

PRINT "=== API Audit: Zanna.Crypto.KeyDerive ==="

' Create a salt
DIM salt AS OBJECT = Zanna.Crypto.SecureRandom.Bytes(16)
PRINT "Generated 16-byte salt"
PRINT "Salt length: "; Zanna.Collections.Bytes.get_Length(salt)

' --- Pbkdf2SHA256 (returns bytes) ---
PRINT "--- Pbkdf2SHA256 ---"
DIM derived AS OBJECT = Zanna.Crypto.KeyDerive.Pbkdf2Sha256("mypassword", salt, 100000, 32)
PRINT "Derived key length: "; Zanna.Collections.Bytes.get_Length(derived)

' --- Pbkdf2SHA256Str (returns hex string) ---
PRINT "--- Pbkdf2SHA256Str ---"
DIM derivedStr AS STRING = Zanna.Crypto.KeyDerive.Pbkdf2Sha256Encoded("mypassword", salt, 100000, 32)
PRINT "Derived key (hex): "; derivedStr

' --- Same password + salt => same result ---
PRINT "--- Determinism check ---"
DIM derivedStr2 AS STRING = Zanna.Crypto.KeyDerive.Pbkdf2Sha256Encoded("mypassword", salt, 100000, 32)
PRINT "Same inputs produce same output: "; derivedStr = derivedStr2

' --- Different password => different result ---
PRINT "--- Different password ---"
DIM derivedStr3 AS STRING = Zanna.Crypto.KeyDerive.Pbkdf2Sha256Encoded("otherpassword", salt, 100000, 32)
PRINT "Different password: "; derivedStr3
PRINT "Different from first: "; derivedStr <> derivedStr3

' --- Different salt => different result ---
PRINT "--- Different salt ---"
DIM salt2 AS OBJECT = Zanna.Crypto.SecureRandom.Bytes(16)
DIM derivedStr4 AS STRING = Zanna.Crypto.KeyDerive.Pbkdf2Sha256Encoded("mypassword", salt2, 100000, 32)
PRINT "Different salt: "; derivedStr4
PRINT "Different from first: "; derivedStr <> derivedStr4

' --- Different key length ---
PRINT "--- Different key length (64 bytes) ---"
DIM derived64 AS OBJECT = Zanna.Crypto.KeyDerive.Pbkdf2Sha256("mypassword", salt, 100000, 64)
PRINT "Key length: "; Zanna.Collections.Bytes.get_Length(derived64)

' --- Different iterations ---
PRINT "--- Different iterations (200000) ---"
DIM derivedStr5 AS STRING = Zanna.Crypto.KeyDerive.Pbkdf2Sha256Encoded("mypassword", salt, 200000, 32)
PRINT "200000 iterations: "; derivedStr5
PRINT "Different from 100000 iterations: "; derivedStr <> derivedStr5

PRINT "=== KeyDerive Audit Complete ==="
END
