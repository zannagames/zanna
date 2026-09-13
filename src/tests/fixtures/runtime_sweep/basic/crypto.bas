' EXPECT_OUT: RESULT: ok
' COVER: Zanna.Crypto.Legacy.Hash.Crc32
' COVER: Zanna.Crypto.Legacy.Hash.Crc32Bytes
' COVER: Zanna.Crypto.Legacy.Hash.Md5
' COVER: Zanna.Crypto.Legacy.Hash.Md5Bytes
' COVER: Zanna.Crypto.Legacy.Hash.Sha1
' COVER: Zanna.Crypto.Legacy.Hash.Sha1Bytes
' COVER: Zanna.Crypto.Hash.Sha256
' COVER: Zanna.Crypto.Hash.Sha256Bytes
' COVER: Zanna.Crypto.Legacy.Hash.HmacMd5
' COVER: Zanna.Crypto.Legacy.Hash.HmacMd5Bytes
' COVER: Zanna.Crypto.Legacy.Hash.HmacSha1
' COVER: Zanna.Crypto.Legacy.Hash.HmacSha1Bytes
' COVER: Zanna.Crypto.Hash.HmacSha256
' COVER: Zanna.Crypto.Hash.HmacSha256Bytes
' COVER: Zanna.Crypto.KeyDerive.Pbkdf2Sha256
' COVER: Zanna.Crypto.KeyDerive.Pbkdf2Sha256Encoded
' COVER: Zanna.Crypto.SecureRandom.Bytes
' COVER: Zanna.Crypto.SecureRandom.Int

DIM data AS STRING
data = "Hello, World!"

DIM dataBytes AS Zanna.Collections.Bytes
dataBytes = Zanna.Collections.Bytes.FromStr(data)

Zanna.Core.Diagnostics.AssertEq(Zanna.Crypto.Legacy.Hash.Crc32(data), 3964322768, "crc32")
Zanna.Core.Diagnostics.AssertEq(Zanna.Crypto.Legacy.Hash.Crc32Bytes(dataBytes), 3964322768, "crc32bytes")
Zanna.Core.Diagnostics.AssertEqStr(Zanna.Crypto.Legacy.Hash.Md5(data), "65a8e27d8879283831b664bd8b7f0ad4", "md5")
Zanna.Core.Diagnostics.AssertEqStr(Zanna.Crypto.Legacy.Hash.Sha1(data), "0a0a9f2a6772942557ab5355d76af442f8f65e01", "sha1")
Zanna.Core.Diagnostics.AssertEqStr(Zanna.Crypto.Hash.Sha256(data), "dffd6021bb2bd5b0af676290809ec3a53191dd81c7f70a4b28688a362182986f", "sha256")
Zanna.Core.Diagnostics.AssertEqStr(Zanna.Crypto.Legacy.Hash.Md5Bytes(dataBytes), "65a8e27d8879283831b664bd8b7f0ad4", "md5bytes")
Zanna.Core.Diagnostics.AssertEqStr(Zanna.Crypto.Legacy.Hash.Sha1Bytes(dataBytes), "0a0a9f2a6772942557ab5355d76af442f8f65e01", "sha1bytes")
Zanna.Core.Diagnostics.AssertEqStr(Zanna.Crypto.Hash.Sha256Bytes(dataBytes), "dffd6021bb2bd5b0af676290809ec3a53191dd81c7f70a4b28688a362182986f", "sha256bytes")

DIM keyStr AS STRING
DIM msgStr AS STRING
keyStr = "key"
msgStr = "data"

DIM keyBytes AS Zanna.Collections.Bytes
DIM msgBytes AS Zanna.Collections.Bytes
keyBytes = Zanna.Collections.Bytes.FromStr(keyStr)
msgBytes = Zanna.Collections.Bytes.FromStr(msgStr)

Zanna.Core.Diagnostics.AssertEqStr(Zanna.Crypto.Legacy.Hash.HmacMd5(keyStr, msgStr), "9d5c73ef85594d34ec4438b7c97e51d8", "hmac.md5")
Zanna.Core.Diagnostics.AssertEqStr(Zanna.Crypto.Legacy.Hash.HmacSha1(keyStr, msgStr), "104152c5bfdca07bc633eebd46199f0255c9f49d", "hmac.sha1")
Zanna.Core.Diagnostics.AssertEqStr(Zanna.Crypto.Hash.HmacSha256(keyStr, msgStr), "5031fe3d989c6d1537a013fa6e739da23463fdaec3b70137d828e36ace221bd0", "hmac.sha256")
Zanna.Core.Diagnostics.AssertEqStr(Zanna.Crypto.Legacy.Hash.HmacMd5Bytes(keyBytes, msgBytes), "9d5c73ef85594d34ec4438b7c97e51d8", "hmac.md5bytes")
Zanna.Core.Diagnostics.AssertEqStr(Zanna.Crypto.Legacy.Hash.HmacSha1Bytes(keyBytes, msgBytes), "104152c5bfdca07bc633eebd46199f0255c9f49d", "hmac.sha1bytes")
Zanna.Core.Diagnostics.AssertEqStr(Zanna.Crypto.Hash.HmacSha256Bytes(keyBytes, msgBytes), "5031fe3d989c6d1537a013fa6e739da23463fdaec3b70137d828e36ace221bd0", "hmac.sha256bytes")

DIM saltStr AS STRING
saltStr = "salt"
DIM saltBytes AS Zanna.Collections.Bytes
saltBytes = Zanna.Collections.Bytes.FromStr(saltStr)

DIM pbBytes AS Zanna.Collections.Bytes
pbBytes = Zanna.Crypto.KeyDerive.Pbkdf2Sha256("password", saltBytes, 100000, 32)
Zanna.Core.Diagnostics.AssertEq(pbBytes.Length, 32, "pbkdf2.len")
Zanna.Core.Diagnostics.AssertEqStr(Zanna.Collections.Bytes.ToHex(pbBytes), "0394a2ede332c9a13eb82e9b24631604c31df978b4e2f0fbd2c549944f9d79a5", "pbkdf2.hex")
Zanna.Core.Diagnostics.AssertEqStr(Zanna.Crypto.KeyDerive.Pbkdf2Sha256Encoded("password", saltBytes, 100000, 32), "0394a2ede332c9a13eb82e9b24631604c31df978b4e2f0fbd2c549944f9d79a5", "pbkdf2.str")

DIM randBytes AS Zanna.Collections.Bytes
randBytes = Zanna.Crypto.SecureRandom.Bytes(16)
Zanna.Core.Diagnostics.AssertEq(randBytes.Length, 16, "rand.bytes")

DIM r AS INTEGER
r = Zanna.Crypto.SecureRandom.Int(1, 6)
Zanna.Core.Diagnostics.Assert(r >= 1 AND r <= 6, "rand.int")

PRINT "RESULT: ok"
END
