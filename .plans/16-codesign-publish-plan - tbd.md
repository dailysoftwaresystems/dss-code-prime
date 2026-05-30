# Codesign + Publish Phase — Sub-Plan

> Owns the cryptographic signing and publishing pipeline for every v1 target. In-tree, hermetic: certificates, private keys, provisioning profiles, and entitlements arrive as file inputs; we produce signed, notarized, store-ready artifacts. **No shelling out to `codesign` / `xcrun` / `signtool` / `apksigner` / `jarsigner`** — we own the bytes from source to signed binary. Apple credentials are required only at sign/publish time on a runner that has the cert; local dev never installs xcrun.

## 0. Status (snapshot)

| | |
|---|---|
| Status        | ⏳ **planned.** v1 production-shipping-critical. Closes the "no external tools" invariant across the final-mile signing step. |
| Predecessors  | ✅ Linker phase ([`14-linker-plan - tbd.md`](./14-linker-plan%20-%20tbd.md)) **LK7 landed 2026-05-30** — reserves the `LC_CODE_SIGNATURE` load command for Mach-O (schema field `image.codeSignatureSize`) and the `IMAGE_DIRECTORY_ENTRY_SECURITY` directory entry for PE (schema field `optionalHeader.attributeCertReserveSize`); both produce zero-filled regions at 8-byte-aligned offsets that CS2 / CS5 patch in place. Debug-info ([`15-debug-info-plan - tbd.md`](./15-debug-info-plan%20-%20tbd.md)) settles which sections fall under the signature. |
| Successors    | None inside the compiler pipeline — codesign + publish is the terminal phase. Distribution / install layers (post-v1) consume the signed artifacts unchanged. |
| Scope         | **Bounded.** CS1 cryptographic substrate. CS2–CS3 Apple codesign + notarization + stapling. CS4 iOS provisioning profile + `.app` bundle. CS5–CS6 Windows Authenticode + RFC 3161 timestamping. CS7 Android APK v3 (skeleton — full impl post-v1). CS8 `publish` driver subcommand. CS9 end-to-end CI signing tests. Linux package signing reserved post-v1. |

---

## 1. Motivation

Production shipping is not "build a binary"; it is "build a binary the OS will execute without a Gatekeeper / SmartScreen / Play-Protect block." Every consumer target in v1 demands cryptographic signatures:

- **Apple (macOS + iOS).** Gatekeeper rejects unsigned Mach-O. Notarization (Apple's malware scan + ticket) is required for distribution outside the App Store. iOS additionally requires a provisioning profile pinned to team-id + bundle-id.
- **Windows.** Unsigned PE binaries trigger SmartScreen "unrecognized publisher" warnings; enterprise IT policy commonly blocks them. Authenticode + an RFC 3161 timestamp from a trusted TSA is the production baseline.
- **Android.** Play Store mandates APK v3 signing (forward-compatible with key rotation). v1 doesn't ship Android binaries, but the signing format must be enumerated now so the substrate doesn't foreclose it.
- **Linux.** Optional — distros consume detached `.sig` / `.asc` files for deb / rpm. Reserved post-v1.

The hermetic invariant compounds the problem. Every other compiler shells out at this step: clang invokes `codesign`, MSVC invokes `signtool`. We do not. The user mandate (per `00-master.md`): "Apple platform will only be needed in the final stage (Signing + Publishing), local development will not need apple at all to work." That means we must own the **wire-level cryptographic structures**: PKCS#7/CMS SignedData, the Apple Code Directory blob, the PE attribute certificate table, the App Store Connect REST endpoints. The signing code paths are ours; the credentials are inputs we accept.

A hermetic, in-tree codesigner is also the only way to guarantee reproducibility: external `codesign` / `signtool` versions drift, embed timestamps, and produce diff-noisy outputs. Our signer is deterministic given the same cert + same input bytes + same fixed RFC 3161 round-trip.

---

## 2. Design

### 2.1 Files

```
src/codesign/
├── CMakeLists.txt
├── crypto/                         # CS1 — cryptographic substrate
│   ├── sha256.hpp / .cpp           # SHA-256, SHA-384
│   ├── der_encoder.hpp / .cpp      # ASN.1 DER writer
│   ├── der_parser.hpp / .cpp       # ASN.1 DER reader (cert chain ingestion)
│   ├── x509.hpp / .cpp             # X.509 cert + chain
│   ├── pkcs7.hpp / .cpp            # PKCS#7 / CMS SignedData builder
│   ├── rsa_signer.hpp / .cpp       # RSA-PKCS1-v1_5 + RSA-PSS
│   ├── ecdsa_signer.hpp / .cpp     # ECDSA P-256 / P-384
│   └── https_client.hpp / .cpp     # TLS 1.2/1.3 client (notarization + TSA)
├── apple/
│   ├── code_directory.hpp / .cpp   # CS2 — page-hash table + Code Directory blob
│   ├── embedded_signature.hpp / .cpp # CS2 — SuperBlob layout, LC_CODE_SIGNATURE fill
│   ├── entitlements.hpp / .cpp     # CS2 — XML plist embedding + hash
│   ├── requirements.hpp / .cpp     # CS2 — designated-requirement DER
│   ├── notary_client.hpp / .cpp    # CS3 — App Store Connect notarization REST
│   ├── ticket_stapler.hpp / .cpp   # CS3 — embed ticket back into binary
│   ├── provisioning_profile.hpp / .cpp # CS4 — embedded.mobileprovision
│   └── app_bundle.hpp / .cpp       # CS4 — .app directory + Info.plist + CodeResources
├── windows/
│   ├── authenticode.hpp / .cpp     # CS5 — PKCS#7 + SpcIndirectDataContent
│   ├── pe_security.hpp / .cpp      # CS5 — PE attribute cert table fill
│   └── rfc3161_tsa.hpp / .cpp      # CS6 — TSA counter-signature client
├── android/
│   └── apk_v3.hpp / .cpp           # CS7 — APK v3 signing block (skeleton)
└── publish/
    ├── publish_driver.hpp / .cpp   # CS8 — driver-side `publish` subcommand
    ├── credential_source.hpp / .cpp # CS8 — file / env / future PKCS#11
    └── diagnostic_redaction.hpp / .cpp # CS8 — never log key material

tests/codesign/
├── CMakeLists.txt
├── crypto/                          # CS1 byte-for-byte tests against RFC vectors
├── apple/                           # CS2–CS4 golden CMS bytes + bundle layout
├── windows/                         # CS5–CS6 PE + TSA counter-signature pins
├── android/                         # CS7 skeleton pin
└── publish/                         # CS8 driver smoke + redaction pin
```

`src/codesign/` is a new top-level peer to `src/codegen/` and `src/linker/`. It runs **after** the linker has produced an artifact with placeholder signature load commands / directory entries; codesign fills those placeholders in place.

### 2.2 Apple codesign (Mach-O)

The Mach-O code signature is a **SuperBlob** appended to the binary, addressed by an `LC_CODE_SIGNATURE` load command the linker reserved. Layout:

```
SuperBlob
├── CodeDirectory blob
│   ├── version + flags + identifier ("com.example.MyApp")
│   ├── team-id ("ABCDEF1234")
│   ├── pageSize = 4 KiB
│   ├── nCodeSlots = ceil(__TEXT_size / 4 KiB)
│   ├── nSpecialSlots = 5..7 (entitlements / requirements / resources / app-specific)
│   ├── hashType = SHA-256
│   ├── entitlements hash      (special slot −5)
│   ├── requirements hash      (special slot −2)
│   ├── resources hash         (special slot −3)
│   └── page-hash table        (slot 0 .. nCodeSlots−1)  — SHA-256(page_i)
├── Requirements blob          (CMS-style internal expression — designated requirement)
├── Entitlements blob          (XML plist, verbatim)
└── CMS signature blob         (PKCS#7 SignedData wrapping CodeDirectory hash)
```

**Page-hash table.** SHA-256 of each 4 KiB page of the `__TEXT` segment (and the Mach-O header + load commands up to the signature). The signature region itself is excluded — the binary is the union of "everything before the signature" + "signature blob."

**CMS signature.** A PKCS#7 SignedData structure whose signed content is the SHA-256 of the CodeDirectory blob. Signer cert chain: leaf "Developer ID Application" cert → Apple Worldwide Developer Relations CA → Apple Root CA. Cert chain is ingested from the user's `.p12` (PKCS#12) bundle; we parse PKCS#12 via the CS1 substrate.

**Reference.** Apple's `cs_blobs.h` defines the wire format; reverse-engineered docs (LLVM's `llvm-objcopy` emits dummy code signatures for cross-compile parity) and Jonathan Levin's *MacOS Internals* cover the rest. We emit **real** signatures, not the LLVM placeholder.

### 2.3 Apple notarization

After signing, Apple-distributed artifacts must be notarized:

1. **Package.** Wrap the signed binary into `.dmg`, `.pkg`, or `.zip`.
2. **Submit.** `POST /v1/notarizations` to App Store Connect with a JWT bearer auth token built from the user's App Store Connect API key (`issuer-id` + `key-id` + ECDSA P-256 `.p8` private key signed via CS1).
3. **Poll.** `GET /v1/notarizations/{submissionId}` every 30s until `status == "Accepted"` or `"Invalid"`.
4. **Staple.** On accept, `GET` the notarization ticket and embed it into the Mach-O as an extension to the existing `LC_CODE_SIGNATURE` SuperBlob (a new "ticket" slot).

The notarization REST shape is documented in Apple's `notarytool` source; we re-implement it. Retry policy: exponential backoff 30s/60s/120s/240s then abort with `G_NOTARIZE_TIMEOUT`.

### 2.4 iOS provisioning profile

For iOS targets, the `.app` bundle must include `embedded.mobileprovision` (a CMS-wrapped plist) at the bundle root. The profile pins the team-id + bundle-id + entitlements; the Code Directory's identifier and team-id must match. CS4 copies the profile into the bundle and validates the pin at sign time, failing with `G_PROFILE_MISMATCH` on divergence.

### 2.5 Windows Authenticode (PE)

The PE `IMAGE_DIRECTORY_ENTRY_SECURITY` directory points at one or more `WIN_CERTIFICATE` records appended to the file. Each record is a PKCS#7 SignedData wrapping a `SpcIndirectDataContent` structure whose digest is **SHA-256 of the PE bytes excluding (a) the checksum field, (b) the security directory entry itself, and (c) the appended cert table**. This three-region exclusion is the historical foot-gun: we pin it byte-for-byte in CS5 tests.

Signer cert + chain ingested from `.pfx` / `.p12`. EV (Extended Validation) certs commonly require a hardware token (HSM) and are out of v1 scope; v1 supports file-based certs only, documented at the boundary.

### 2.6 Windows RFC 3161 timestamping

The Authenticode signature is counter-signed by a Microsoft-trusted TSA so the signature remains valid after the signing cert expires. We POST a TimeStampReq (DER) to a TSA URL (configurable, e.g. `http://timestamp.digicert.com`) over HTTPS, get back a TimeStampResp, and embed the resulting TST as an unauthenticated attribute on the PKCS#7 SignerInfo. RFC 3161 wire format is implemented in CS1's DER layer.

### 2.7 Android APK v3 (skeleton, post-v1 full impl)

APK v3 inserts a signed-data block between the last entry and the central directory of the ZIP. The block contains:

- Per-signer info (cert + signature algorithm)
- SHA-256 hash of each 1 MiB chunk of (file entries, central directory, EOCD with offsets adjusted)
- Rotated-key proof (v3-specific: lineage of past signing keys signed by the current key)

CS7 lands the wire-format skeleton + arena types so the v1 substrate doesn't foreclose APK signing when Android joins the target matrix. Full implementation + Play Store integration is post-v1.

### 2.8 Linux signing (post-v1)

Detached `.sig` files (deb GPG, rpm GPG) reserved post-v1. ELF in-binary signatures are not standardized; the kernel module signing format (`module-sig`) is the closest precedent and is out of scope for v1.

### 2.9 Driver integration

`dss-code-prime` gains a `publish` subcommand:

```
dss-code-prime publish --target apple-store ./MyApp.app \
    --cert ./dev-id.p12 \
    --key-pass ENV:CERT_PASS \
    --notarize \
    --apple-api-key ./AuthKey.p8 \
    --apple-issuer-id ABCDEF12-3456-7890-ABCD-EF1234567890 \
    --apple-key-id XYZ1234567

dss-code-prime publish --target microsoft-store ./MyApp.exe \
    --cert ./auth.pfx \
    --key-pass ENV:CERT_PASS \
    --timestamp-url http://timestamp.digicert.com
```

Credential sources (CS8): file path, `ENV:VAR` indirection, future `PKCS11:slot=N` indirection (post-v1). Diagnostic redaction (CS8): the verbose log records cert subject + serial + SHA-256 fingerprint, never the raw bytes; passwords never appear in any diagnostic; the `G_CREDENTIAL_REDACTED` tracer pin asserts redaction at every log site.

### 2.10 Cryptographic substrate (CS1)

The substrate is **library-level**, not algorithm-bespoke. We need: SHA-256, SHA-384, X.509 / DER parse + emit, PKCS#7/CMS SignedData build, RSA (PKCS1-v1_5 + PSS) signer, ECDSA (P-256 + P-384) signer, TLS 1.2/1.3 HTTPS client. **Default decision (see §4 Q1): vendor BearSSL** — ~600 KB, MIT, audited, no allocator surprises, integrates cleanly into our hermetic build. Crypto is too high-stakes to roll from scratch; vendoring a battle-tested lib is the only responsible choice. We **wrap** BearSSL behind our own `dss::codesign::crypto` headers so the rest of the codebase never sees BearSSL types directly — if we ever swap (e.g. to libsodium), the swap is contained.

### 2.11 Diagnostics

All codesign / publish diagnostics use the `G_*` namespace (per `07-production-readiness-plan`):

- `G_SIGN_OK` — info, signature emitted, fingerprint logged
- `G_SIGN_CERT_EXPIRED` — error, cert past `notAfter`
- `G_SIGN_CERT_REVOKED` — error, OCSP / CRL says revoked (post-v1; v1 skips revocation check)
- `G_NOTARIZE_SUBMITTED` — info, submission id logged
- `G_NOTARIZE_REJECTED` — error, Apple returned `Invalid` with reasons
- `G_NOTARIZE_TIMEOUT` — error, exponential backoff exhausted
- `G_AUTHENTICODE_OK` — info
- `G_TSA_UNREACHABLE` — error, TSA HTTP failed after retries
- `G_PROFILE_MISMATCH` — error, provisioning profile team-id / bundle-id != code directory
- `G_CREDENTIAL_REDACTED` — internal tracer, asserts no raw key material logged

Every error carries a remediation hint (cert paths, regen instructions, Apple support URLs).

---

## 3. PR breakdown

| PR  | Scope                                                                                                | Closes                  |
|-----|------------------------------------------------------------------------------------------------------|-------------------------|
| CS1 | Crypto substrate: vendor + wrap BearSSL (SHA-256/384, DER, X.509, PKCS#7/CMS, RSA, ECDSA, HTTPS).    | substrate gate          |
| CS2 | Apple codesign: page-hash table + Code Directory + Entitlements + Requirements + SuperBlob + LC fill. | macOS / iOS sign        |
| CS3 | Apple notarization HTTP client + ticket stapling.                                                    | macOS distribution      |
| CS4 | iOS provisioning profile embedding + `.app` bundle assembler + `Info.plist` + `_CodeSignature/CodeResources`. | iOS distribution |
| CS5 | Windows Authenticode: PE security directory fill + PKCS#7 + `SpcIndirectDataContent`.                | Windows sign            |
| CS6 | Windows RFC 3161 TSA client + counter-signature embedding.                                           | Windows production sign |
| CS7 | Android APK v3 skeleton: arena types + wire-format pin (full impl post-v1).                          | Android foreclosure     |
| CS8 | `dss-code-prime publish` driver subcommand + credential sourcing + diagnostic redaction.             | driver integration      |
| CS9 | End-to-end CI signing tests: self-signed CA for dev tests, real-cert nightly verification at release. | acceptance              |

Sequencing: CS1 lands first (substrate gate). CS2 + CS5 can land in parallel (independent platforms). CS3 follows CS2. CS4 follows CS2. CS6 follows CS5. CS7 stands alone. CS8 lands after CS2 + CS5 minimum. CS9 lands last.

---

## 4. Open questions

| #  | Question                                                                              | Default                                                                                              |
|----|---------------------------------------------------------------------------------------|------------------------------------------------------------------------------------------------------|
| Q1 | Vendor a crypto lib or write the subset?                                              | **Vendor BearSSL** (~600 KB, MIT, audited). Crypto is too high-stakes to write from scratch. Wrap behind our headers so swap-out is contained. |
| Q2 | Hardware-token / HSM support (PKCS#11)?                                               | Post-v1. File-based cert only in v1.                                                                 |
| Q3 | Apple App Store upload (`xcrun altool`-replacement)?                                  | Notarization only in v1. Full store-upload post-v1.                                                  |
| Q4 | Linux package signing (deb / rpm GPG)?                                                | Post-v1.                                                                                             |
| Q5 | Secrets management for CI?                                                            | Env-var-injected credentials in v1; per-platform secret stores (Azure KV, AWS Secrets Manager) post-v1. |
| Q6 | Notarization retry policy on transient HTTP failures?                                 | Exponential backoff 30s / 60s / 120s / 240s, then abort with `G_NOTARIZE_TIMEOUT`.                   |
| Q7 | Revocation check (OCSP / CRL) at sign time?                                           | Post-v1. v1 logs cert `notAfter` and refuses expired certs but does not contact OCSP.                |
| Q8 | Reproducible signatures (same input + same cert + same timestamp ⇒ identical bytes)?  | Yes for the codesign portion; the RFC 3161 timestamp introduces unavoidable drift, isolated to the TST attribute. |

---

## 5. Acceptance

- [ ] CS1 substrate passes RFC test vectors: SHA-256 (FIPS 180-4), RSA PKCS1-v1_5 (RFC 8017), ECDSA P-256 (FIPS 186-4), DER encoding (round-trip pins).
- [ ] CS2: self-signed test cert produces a signed `.app` whose SuperBlob parses cleanly with `llvm-objcopy --dump-section __LINKEDIT.code_signature` (oracle role only — never invoked at runtime).
- [ ] CS3: nightly CI happy-path against Apple's notarization sandbox — submit, poll, staple — produces a notarized binary that Gatekeeper accepts on a real macOS runner.
- [ ] CS4: signed `.app` bundle layout matches Apple's expected structure (`_CodeSignature/CodeResources` hash table verified by recomputation).
- [ ] CS5: signed PE passes PowerShell `Get-AuthenticodeSignature` verification with a test cert; structure cross-validated by `signtool verify /pa` in oracle role.
- [ ] CS6: counter-signed PE has a valid TST attribute that `signtool verify /pa /tw` accepts (oracle).
- [ ] CS7: APK v3 skeleton emits a parseable signed-data block (Google's `apksigner verify --print-certs` accepts the block in oracle role).
- [ ] CS8: `dss-code-prime --verbose publish ...` log scraped for raw key material — zero hits across `.p12`, `.pfx`, `.p8`, and `--key-pass` value bytes.
- [ ] CS9: hermetic CI runner has **no** `codesign`, `xcrun`, `signtool`, `apksigner`, or `jarsigner` installed; full publish pipeline passes end-to-end. This is enforced by a pre-flight check that fails the build if any of those binaries are present on the runner.
- [ ] All diagnostics use `G_*` namespace; remediation hints present on every error.
- [ ] No external tool invoked at runtime — only at test-oracle time, gated behind `DSS_TEST_USE_EXTERNAL_ORACLES=ON` cmake flag, default OFF.

---

## 6. Risks

| Risk                                                                                                   | Mitigation                                                                                                  |
|--------------------------------------------------------------------------------------------------------|-------------------------------------------------------------------------------------------------------------|
| Crypto correctness — wrong PKCS#7 byte layout passes our test but fails Apple Gatekeeper silently.     | Golden CMS byte-pin tests against captured-from-real-`codesign` reference signatures; nightly real-cert Gatekeeper verification at release time. |
| Notarization API drift — Apple changes endpoints or response schema.                                   | Version-pin the API call shape; alarm + halt-release on schema mismatch; track Apple's `notarytool` source quarterly. |
| TSA endpoint outage — DigiCert / Sectigo TSA down on release day.                                      | Configurable TSA URL list with automatic failover; alarm on full-list outage; do not silently emit unsigned. |
| Hardware-token requirement for EV certs.                                                               | Document the v1 boundary clearly; EV / HSM is post-v1; v1 ships with OV certs only.                          |
| Credential leak through diagnostic logs.                                                               | `G_CREDENTIAL_REDACTED` tracer + scraping test in CS9 acceptance; redaction is a hard invariant.            |
| BearSSL upstream goes unmaintained.                                                                    | Wrap behind our headers; vendor a pinned version; swap-out path documented in CS1.                          |
| Reproducibility drift via TSA timestamp.                                                               | Isolate the TST attribute as the **only** non-reproducible region; pin everything else byte-for-byte.        |

---

## 7. Sequencing

- **Hard dependency:** linker phase [`14-linker-plan - tbd.md`](./14-linker-plan%20-%20tbd.md) reserves `LC_CODE_SIGNATURE` (LK7) for Mach-O and the `IMAGE_DIRECTORY_ENTRY_SECURITY` table for PE. Codesign fills those placeholders in place; without them the linker emits unsignable artifacts.
- **Hard dependency:** debug-info phase [`15-debug-info-plan - tbd.md`](./15-debug-info-plan%20-%20tbd.md) settles which sections (`__DWARF` / `.debug_*`) are inside vs. outside the signature region. Apple covers debug sections; Windows convention varies.
- **Loose dependency:** driver / `program-api` for the `publish` subcommand wiring (CS8). The driver-side surface is small; can land alongside CS8 without blocking earlier PRs.
- **Loose dependency:** artifact-profile plan [`06-artifact-profile-plan - tbd.md`](./06-artifact-profile-plan%20-%20tbd.md) defines `lib` vs. `cli` vs. `script`; codesign cares which artifact profiles require signing (cli + dylib yes; pure script no).
- **No dependency:** semantic / IR / codegen phases. Codesign operates on linked artifacts; the upstream pipeline shape is opaque to it.
- **Terminal:** nothing in the compiler pipeline depends on codesign output. Distribution / install layers (post-v1) consume signed artifacts unchanged.

Runs alongside [`15-debug-info-plan - tbd.md`](./15-debug-info-plan%20-%20tbd.md); both gated on [`14-linker-plan - tbd.md`](./14-linker-plan%20-%20tbd.md) reservations.
