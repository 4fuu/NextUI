# Test-only fixtures

`test-ed25519-seed.hex` is a fixed 32-byte seed used only to reproduce the
Ed25519 test key and the fixed signature in `package/`.

It is deliberately committed so the protocol test is deterministic. It is not a
production key, is not used by any release workflow, and must never sign a real
update.

`test-public-key` is the matching `nextui-public-key-v1` sidecar. The fixed
package, manifest, and signature exercise the verifier independently of the
manifest generator at test time.
