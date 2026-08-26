# Public offline DSSE verification fixture

This CC0-1.0 fixture was generated once for M20-PRE-01 with OpenSSL 3 using a
temporary 2048-bit RSA key. The retained files contain only a small public
artifact, a canonical in-toto Statement v1 inside a DSSE envelope, its
RSASSA-PKCS1-v1_5-SHA256 signature, and the public modulus/exponent. The
private key was not retained and is not needed by the verifier.

The statement describes the fictional public repository
`example/portable-attestation-fixture` at a fixed source digest and tag. It is
test material, not an RTFW build, release, signer authorization, or current
revocation record. Positive verification establishes only that the retained
signature matches the retained public key and that the exact fixture identity
policy matches. Negative tests mutate the artifact, payload, signature, trust
material, repository, source/ref, workflow, issuer, and predicate.
