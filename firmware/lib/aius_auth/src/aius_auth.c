/* Aius physical-access bootstrap authorization. Portable C, no heap. */

#include "aius_auth.h"

/* This is a compiled, public salt for domain separation, not a secret key. */
static const uint8_t k_compiled_salt[] =
	"AIUS-P7-physical-access-bootstrap-v1";

static uint64_t fnv1a64_byte(uint64_t state, uint8_t value)
{
	state ^= (uint64_t)value;
	return state * UINT64_C(1099511628211);
}

static uint64_t derive_value(
	const uint8_t device_material[AIUS_AUTH_DEVICE_MATERIAL_SIZE])
{
	uint64_t state = UINT64_C(14695981039346656037);

	for (size_t i = 0; i < sizeof(k_compiled_salt); i++)
		state = fnv1a64_byte(state, k_compiled_salt[i]);
	for (size_t i = 0; i < AIUS_AUTH_DEVICE_MATERIAL_SIZE; i++)
		state = fnv1a64_byte(state, device_material[i]);
	return state;
}

AiusAuthStatus aius_auth_format_token(
	uint64_t value, char token[AIUS_AUTH_TOKEN_STORAGE_SIZE])
{
	static const char hex[] = "0123456789abcdef";

	if (token == NULL)
		return AIUS_AUTH_ERR_ARGUMENT;
	for (size_t i = 0; i < AIUS_AUTH_TOKEN_LENGTH; i++) {
		size_t shift = (AIUS_AUTH_TOKEN_LENGTH - 1u - i) * 4u;
		token[i] = hex[(value >> shift) & UINT64_C(0xf)];
	}
	token[AIUS_AUTH_TOKEN_LENGTH] = '\0';
	return AIUS_AUTH_OK;
}

AiusAuthStatus aius_auth_derive_token(
	const uint8_t device_material[AIUS_AUTH_DEVICE_MATERIAL_SIZE],
	char token[AIUS_AUTH_TOKEN_STORAGE_SIZE])
{
	if (device_material == NULL || token == NULL)
		return AIUS_AUTH_ERR_ARGUMENT;
	return aius_auth_format_token(derive_value(device_material), token);
}

static uint8_t is_hex(uint8_t value)
{
	return (uint8_t)(((value >= (uint8_t)'0') && (value <= (uint8_t)'9')) ||
	                  ((value >= (uint8_t)'a') && (value <= (uint8_t)'f')) ||
	                  ((value >= (uint8_t)'A') && (value <= (uint8_t)'F')));
}

static uint8_t lower_hex(uint8_t value)
{
	uint8_t upper = (uint8_t)((value >= (uint8_t)'A') &&
	                          (value <= (uint8_t)'F'));
	return (uint8_t)(value + (uint8_t)(upper * ((uint8_t)'a' - (uint8_t)'A')));
}

AiusAuthStatus aius_auth_validate_token(const char *token, size_t length)
{
	uint32_t invalid = (uint32_t)(length != AIUS_AUTH_TOKEN_LENGTH);

	if (token == NULL)
		return AIUS_AUTH_ERR_ARGUMENT;
	for (size_t i = 0; i < AIUS_AUTH_TOKEN_LENGTH; i++) {
		uint8_t value = 0u;
		if (i < length)
			value = (uint8_t)token[i];
		invalid |= (uint32_t)(is_hex(value) == 0u);
	}
	if (length != AIUS_AUTH_TOKEN_LENGTH)
		return AIUS_AUTH_ERR_LENGTH;
	return invalid == 0u ? AIUS_AUTH_OK : AIUS_AUTH_ERR_CONTENT;
}

int aius_auth_token_matches(
	const char expected[AIUS_AUTH_TOKEN_LENGTH], const char *presented,
	size_t presented_length)
{
	uint32_t difference = (uint32_t)(presented_length != AIUS_AUTH_TOKEN_LENGTH);

	if (expected == NULL || presented == NULL)
		return 0;
	for (size_t i = 0; i < AIUS_AUTH_TOKEN_LENGTH; i++) {
		uint8_t expected_value = (uint8_t)expected[i];
		uint8_t presented_value = 0u;
		if (i < presented_length)
			presented_value = (uint8_t)presented[i];
		difference |= (uint32_t)(is_hex(expected_value) == 0u);
		difference |= (uint32_t)(is_hex(presented_value) == 0u);
		difference |= (uint32_t)(lower_hex(expected_value) ^
		                         lower_hex(presented_value));
	}
	return difference == 0u;
}
