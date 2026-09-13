' test_crypto.bas — Zanna.Crypto.Hash + Rand + Password
PRINT Zanna.Crypto.Legacy.Hash.Md5("hello")
PRINT Zanna.Crypto.Legacy.Hash.Sha1("hello")
PRINT Zanna.Crypto.Hash.Sha256("hello")
PRINT Zanna.Crypto.Legacy.Hash.Crc32("hello")
PRINT Zanna.Crypto.Legacy.Hash.HmacMd5("hello", "key")
PRINT Zanna.Crypto.Legacy.Hash.HmacSha1("hello", "key")
PRINT Zanna.Crypto.Hash.HmacSha256("hello", "key")

DIM r1 AS INTEGER
LET r1 = Zanna.Crypto.SecureRandom.Int(1, 100)
PRINT r1 > 0
PRINT r1 < 101

DIM rb AS OBJECT
LET rb = Zanna.Crypto.SecureRandom.Bytes(16)
PRINT rb.Length

DIM pw AS STRING
LET pw = Zanna.Crypto.Password.Hash("secret")
PRINT Zanna.String.Contains(pw, "$")
PRINT Zanna.Crypto.Password.Verify("secret", pw)
PRINT Zanna.Crypto.Password.Verify("wrong", pw)

PRINT "done"
END
