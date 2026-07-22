#include "aius_auth.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static void test_deterministic_derivation(void)
{
	const uint8_t material[AIUS_AUTH_DEVICE_MATERIAL_SIZE] = {
		0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
		0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff
	};
	uint8_t other_material[AIUS_AUTH_DEVICE_MATERIAL_SIZE];
	char first[AIUS_AUTH_TOKEN_STORAGE_SIZE];
	char second[AIUS_AUTH_TOKEN_STORAGE_SIZE];
	char other[AIUS_AUTH_TOKEN_STORAGE_SIZE];

	memcpy(other_material, material, sizeof(other_material));
	other_material[0] ^= 1u;
	assert(aius_auth_derive_token(material, first) == AIUS_AUTH_OK);
	assert(aius_auth_derive_token(material, second) == AIUS_AUTH_OK);
	assert(aius_auth_derive_token(other_material, other) == AIUS_AUTH_OK);
	assert(strcmp(first, second) == 0);
	assert(strcmp(first, other) != 0);
	assert(strlen(first) == AIUS_AUTH_TOKEN_LENGTH);
	assert(strcmp(first, "e360e564a72368b9") == 0);
	assert(aius_auth_validate_token(first, AIUS_AUTH_TOKEN_LENGTH) == AIUS_AUTH_OK);
}

static void test_formatting(void)
{
	char token[AIUS_AUTH_TOKEN_STORAGE_SIZE];

	assert(aius_auth_format_token(UINT64_C(0x0123456789abcdef), token) ==
	       AIUS_AUTH_OK);
	assert(strcmp(token, "0123456789abcdef") == 0);
	assert(aius_auth_format_token(UINT64_C(0), token) == AIUS_AUTH_OK);
	assert(strcmp(token, "0000000000000000") == 0);
	assert(aius_auth_format_token(UINT64_MAX, token) == AIUS_AUTH_OK);
	assert(strcmp(token, "ffffffffffffffff") == 0);
	assert(aius_auth_format_token(UINT64_C(1), NULL) == AIUS_AUTH_ERR_ARGUMENT);
}

static void test_validation_rejects_wrong_length_and_content(void)
{
	static const char valid[] = "0123456789abcdef";
	static const char upper[] = "0123456789ABCDEF";
	static const char wrong_content[] = "0123456789abcdeg";

	assert(aius_auth_validate_token(valid, AIUS_AUTH_TOKEN_LENGTH) == AIUS_AUTH_OK);
	assert(aius_auth_validate_token(upper, AIUS_AUTH_TOKEN_LENGTH) == AIUS_AUTH_OK);
	assert(aius_auth_validate_token(valid, AIUS_AUTH_TOKEN_LENGTH - 1u) ==
	       AIUS_AUTH_ERR_LENGTH);
	assert(aius_auth_validate_token("0123456789abcdef0", AIUS_AUTH_TOKEN_LENGTH + 1u) ==
	       AIUS_AUTH_ERR_LENGTH);
	assert(aius_auth_validate_token(wrong_content, AIUS_AUTH_TOKEN_LENGTH) ==
	       AIUS_AUTH_ERR_CONTENT);
	assert(aius_auth_validate_token(NULL, 0) == AIUS_AUTH_ERR_ARGUMENT);
}

static void test_constant_time_mismatch_contract(void)
{
	static const char expected[] = "0123456789abcdef";
	static const char mismatch[] = "0123456789abcdee";
	static const char invalid[] = "0123456789abcde!";

	assert(aius_auth_token_matches(expected, expected, AIUS_AUTH_TOKEN_LENGTH));
	assert(aius_auth_token_matches(expected, "0123456789ABCDEF",
	                               AIUS_AUTH_TOKEN_LENGTH));
	assert(!aius_auth_token_matches(expected, mismatch, AIUS_AUTH_TOKEN_LENGTH));
	assert(!aius_auth_token_matches(expected, invalid, AIUS_AUTH_TOKEN_LENGTH));
	assert(!aius_auth_token_matches(expected, expected, AIUS_AUTH_TOKEN_LENGTH - 1u));
	assert(!aius_auth_token_matches(expected, expected, AIUS_AUTH_TOKEN_LENGTH + 1u));
	assert(!aius_auth_token_matches(NULL, expected, AIUS_AUTH_TOKEN_LENGTH));
	assert(!aius_auth_token_matches(expected, NULL, 0));
}

int main(void)
{
	test_deterministic_derivation();
	test_formatting();
	test_validation_rejects_wrong_length_and_content();
	test_constant_time_mismatch_contract();
	puts("ok auth");
	return 0;
}
