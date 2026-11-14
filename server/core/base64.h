#pragma once

// Returns a null terminated base64 encoded string.
char *base64_encode(const unsigned char *data, size_t input_length);

// Decodes a padded base64 string. Returns a malloc'd buffer (NULL if the input is invalid) and sets *out_len.
uint8_t *base64_decode(const char *str, size_t len, size_t *out_len);