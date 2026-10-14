#pragma once
// Public update key; private key is local under ignored .tools/.
static const char kUpdatePublicKey[] = R"KEY(-----BEGIN PUBLIC KEY-----
MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAENCLSFemATIm3nal0cLjdVh3E0tsF
f6JXn5kruinNkzdetA+IrqARTPo+MQlUv36+qdshBHgBC1HlTW+8AdgoqQ==
-----END PUBLIC KEY-----
)KEY";
// GitHub repository whose latest release carries the signed updates ("owner/name").
static const char kUpdateRepo[] = "vandalon/ultimatrix";
