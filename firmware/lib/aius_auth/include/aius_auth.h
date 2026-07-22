#ifndef AIUS_AUTH_H
#define AIUS_AUTH_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Physical-access bootstrap authorization for the Aius device.
 *
 * This API deliberately does not claim cryptographic strength. The token is a
 * fixed-format, firmware-derived operator credential for a later transport
 * boundary; it is not a password hash, MAC, or secure secret.
 */
#define AIUS_AUTH_DEVICE_MATERIAL_SIZE 16u
#define AIUS_AUTH_TOKEN_LENGTH 16u
#define AIUS_AUTH_TOKEN_STORAGE_SIZE (AIUS_AUTH_TOKEN_LENGTH + 1u)

typedef enum {
	AIUS_AUTH_OK = 0,
	AIUS_AUTH_ERR_ARGUMENT,
	AIUS_AUTH_ERR_LENGTH,
	AIUS_AUTH_ERR_CONTENT
} AiusAuthStatus;

/*
 * Derive the canonical lower-case hexadecimal token from exactly 128 bits of
 * firmware-held device material. `token` receives 16 characters plus a NUL.
 * No allocation or mutable global state is used.
 */
AiusAuthStatus aius_auth_derive_token(
	const uint8_t device_material[AIUS_AUTH_DEVICE_MATERIAL_SIZE],
	char token[AIUS_AUTH_TOKEN_STORAGE_SIZE]);

/* Format a 64-bit token value as exactly 16 lower-case hexadecimal characters. */
AiusAuthStatus aius_auth_format_token(
	uint64_t value, char token[AIUS_AUTH_TOKEN_STORAGE_SIZE]);

/* Validate the exact token length and hexadecimal content. Upper-case input is
 * accepted as an equivalent presentation, while formatting is lower-case. */
AiusAuthStatus aius_auth_validate_token(const char *token, size_t length);

/*
 * Compare a generated token with a presented value.
 *
 * The comparison visits all 16 token positions and accumulates differences;
 * it does not return early on a mismatching character. A wrong length or
 * non-hexadecimal character is rejected. `expected` points to the 16 token
 * characters (a trailing NUL is not required).
 */
int aius_auth_token_matches(
	const char expected[AIUS_AUTH_TOKEN_LENGTH], const char *presented,
	size_t presented_length);

#ifdef __cplusplus
}
#endif

#endif /* AIUS_AUTH_H */
