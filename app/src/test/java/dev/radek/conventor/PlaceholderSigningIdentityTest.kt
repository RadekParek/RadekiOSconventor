package dev.radek.conventor

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import java.nio.file.Files
import java.security.Signature

class PlaceholderSigningIdentityTest {
    @Test fun generatedIdentityIsSelfSignedAndStableAcrossBuilds() {
        val directory = Files.createTempDirectory("placeholder-signer").toFile()
        try {
            val file = directory.resolve("signing-identity.bin")
            val first = PlaceholderSigningIdentity.loadOrCreate(file)
            first.certificate.checkValidity()
            first.certificate.verify(first.certificate.publicKey)
            assertEquals("RSA", first.privateKey.algorithm)
            assertTrue(file.isFile)

            val restored = PlaceholderSigningIdentity.loadOrCreate(file)
            assertArrayEquals(first.privateKey.encoded, restored.privateKey.encoded)
            assertArrayEquals(first.certificate.encoded, restored.certificate.encoded)

            val sample = "placeholder APK signing check".toByteArray()
            val signer = Signature.getInstance("SHA256withRSA")
            signer.initSign(restored.privateKey)
            signer.update(sample)
            val signed = signer.sign()
            signer.initVerify(restored.certificate.publicKey)
            signer.update(sample)
            assertTrue(signer.verify(signed))
        } finally {
            directory.deleteRecursively()
        }
    }
}
