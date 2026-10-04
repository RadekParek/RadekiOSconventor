package dev.radek.conventor

import java.io.ByteArrayInputStream
import java.io.ByteArrayOutputStream
import java.io.DataInputStream
import java.io.DataOutputStream
import java.io.File
import java.math.BigInteger
import java.nio.charset.StandardCharsets
import java.security.KeyFactory
import java.security.KeyPairGenerator
import java.security.PrivateKey
import java.security.Signature
import java.security.SecureRandom
import java.security.spec.PKCS8EncodedKeySpec
import java.security.spec.X509EncodedKeySpec
import java.security.cert.CertificateFactory
import java.security.cert.X509Certificate
import java.time.Instant
import java.time.ZoneOffset
import java.time.format.DateTimeFormatter

/** Per-install RSA identity used only to sign generated placeholder APKs. */
internal data class PlaceholderSigningIdentity(
    val privateKey: PrivateKey,
    val certificate: X509Certificate,
) {
    companion object {
        private const val MAGIC = 0x52445031 // RDP1
        private const val MAX_KEY_BYTES = 16 * 1024

        fun loadOrCreate(file: File): PlaceholderSigningIdentity {
            if (file.isFile) return decode(file.readBytes())
            file.parentFile?.mkdirs()
            val keyPair = KeyPairGenerator.getInstance("RSA").apply { initialize(2048) }.generateKeyPair()
            val identity = PlaceholderSigningIdentity(keyPair.private, createCertificate(keyPair.private, keyPair.public.encoded))
            val temporary = File(file.parentFile, file.name + ".tmp")
            val fileOutput = temporary.outputStream()
            DataOutputStream(fileOutput.buffered()).use { output ->
                val privateBytes = keyPair.private.encoded
                val publicBytes = keyPair.public.encoded
                val certificateBytes = identity.certificate.encoded
                require(privateBytes.size <= MAX_KEY_BYTES && publicBytes.size <= MAX_KEY_BYTES && certificateBytes.size <= MAX_KEY_BYTES)
                output.writeInt(MAGIC)
                writeBlob(output, privateBytes)
                writeBlob(output, publicBytes)
                writeBlob(output, certificateBytes)
                output.flush()
                fileOutput.fd.sync()
            }
            if (!temporary.renameTo(file)) {
                temporary.delete()
                if (file.isFile) return decode(file.readBytes())
                error("could not persist placeholder signing identity")
            }
            return identity
        }

        private fun decode(encoded: ByteArray): PlaceholderSigningIdentity {
            require(encoded.size <= MAX_KEY_BYTES * 3 + 64) { "placeholder signing identity is too large" }
            DataInputStream(ByteArrayInputStream(encoded)).use { input ->
                require(input.readInt() == MAGIC) { "invalid placeholder signing identity" }
                val privateBytes = readBlob(input)
                val publicBytes = readBlob(input)
                val certificateBytes = readBlob(input)
                require(input.available() == 0) { "unexpected data in placeholder signing identity" }
                val factory = KeyFactory.getInstance("RSA")
                val privateKey = factory.generatePrivate(PKCS8EncodedKeySpec(privateBytes))
                val publicKey = factory.generatePublic(X509EncodedKeySpec(publicBytes))
                val certificate = CertificateFactory.getInstance("X.509")
                    .generateCertificate(ByteArrayInputStream(certificateBytes)) as X509Certificate
                require(certificate.publicKey.encoded.contentEquals(publicKey.encoded)) {
                    "placeholder signing identity certificate/key mismatch"
                }
                return PlaceholderSigningIdentity(privateKey, certificate)
            }
        }

        private fun createCertificate(privateKey: PrivateKey, publicKeyInfo: ByteArray): X509Certificate {
            val signatureAlgorithm = sequence(
                oid(byteArrayOf(0x2a, 0x86.toByte(), 0x48, 0x86.toByte(), 0xf7.toByte(), 0x0d, 0x01, 0x01, 0x0b)),
                der(0x05, byteArrayOf()),
            )
            val name = sequence(
                der(0x31, sequence(
                    oid(byteArrayOf(0x55, 0x04, 0x03)),
                    der(0x0c, "RadekiOS placeholder signer".toByteArray(StandardCharsets.UTF_8)),
                )),
            )
            val now = Instant.now()
            val formatter = DateTimeFormatter.ofPattern("yyyyMMddHHmmss'Z'").withZone(ZoneOffset.UTC)
            val validity = sequence(
                der(0x18, formatter.format(now.minusSeconds(86400)).toByteArray(StandardCharsets.US_ASCII)),
                der(0x18, formatter.format(now.plusSeconds(60L * 60 * 24 * 365 * 20)).toByteArray(StandardCharsets.US_ASCII)),
            )
            val serial = ByteArray(16).also { SecureRandom().nextBytes(it) }
            val tbs = sequence(
                der(0xa0, der(0x02, byteArrayOf(2))),
                der(0x02, BigInteger(1, serial).toByteArray()),
                signatureAlgorithm,
                name,
                validity,
                name,
                publicKeyInfo,
            )
            val signer = Signature.getInstance("SHA256withRSA")
            signer.initSign(privateKey)
            signer.update(tbs)
            val certificateDer = sequence(
                tbs,
                signatureAlgorithm,
                der(0x03, byteArrayOf(0) + signer.sign()),
            )
            return CertificateFactory.getInstance("X.509")
                .generateCertificate(ByteArrayInputStream(certificateDer)) as X509Certificate
        }

        private fun writeBlob(output: DataOutputStream, bytes: ByteArray) {
            output.writeInt(bytes.size)
            output.write(bytes)
        }

        private fun readBlob(input: DataInputStream): ByteArray {
            val size = input.readInt()
            require(size in 1..MAX_KEY_BYTES && size <= input.available()) { "invalid key/certificate length" }
            return ByteArray(size).also { input.readFully(it) }
        }

        private fun sequence(vararg children: ByteArray): ByteArray = der(0x30, children.fold(ByteArray(0)) { acc, child -> acc + child })

        private fun oid(content: ByteArray): ByteArray = der(0x06, content)

        private fun der(tag: Int, value: ByteArray): ByteArray {
            val output = ByteArrayOutputStream(value.size + 6)
            output.write(tag)
            when {
                value.size < 0x80 -> output.write(value.size)
                value.size <= 0xff -> {
                    output.write(0x81)
                    output.write(value.size)
                }
                value.size <= 0xffff -> {
                    output.write(0x82)
                    output.write(value.size ushr 8)
                    output.write(value.size)
                }
                else -> {
                    output.write(0x83)
                    output.write(value.size ushr 16)
                    output.write(value.size ushr 8)
                    output.write(value.size)
                }
            }
            output.write(value)
            return output.toByteArray()
        }
    }
}
