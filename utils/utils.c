#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

static const char base64_table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// Handle out of memory error:
void handle_oom()
{
    fprintf(stderr, "Out of memory!");
    exit(-1);
}

// returns a null terminated base64 encoded string.
char *base64_encode(const unsigned char *data, size_t input_length)
{

    size_t output_length = 4 * ((input_length + 2) / 3); // 3 bytes become 4 base64 chars
    char *encoded_data = (char *)malloc(output_length + 1);
    if (encoded_data == NULL)
    {
        handle_oom();
    }

    size_t i, j;
    for (i = 0, j = 0; i < input_length;)
    {

        uint32_t octet_a = i < input_length ? data[i++] : 0;
        uint32_t octet_b = i < input_length ? data[i++] : 0;
        uint32_t octet_c = i < input_length ? data[i++] : 0;

        uint32_t triple = (octet_a << 16) | (octet_b << 8) | octet_c;

        encoded_data[j++] = base64_table[(triple >> 18) & 0x3F];
        encoded_data[j++] = base64_table[(triple >> 12) & 0x3F];
        encoded_data[j++] = base64_table[(triple >> 6) & 0x3F];
        encoded_data[j++] = base64_table[triple & 0x3F];
    }

    size_t remainder = input_length % 3;
    if (remainder == 1)
    {
        encoded_data[output_length - 1] = '=';
        encoded_data[output_length - 2] = '=';
    }
    else if (remainder == 2)
    {
        encoded_data[output_length - 1] = '=';
    }

    encoded_data[output_length] = '\0';
    return encoded_data;
}

static int base64_value(char c)
{
    if (c >= 'A' && c <= 'Z')
        return c - 'A';
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 26;
    if (c >= '0' && c <= '9')
        return c - '0' + 52;
    if (c == '+')
        return 62;
    if (c == '/')
        return 63;
    return -1;
}

uint8_t *base64_decode(const char *str, size_t len, size_t *out_len)
{
    if (len == 0 || len % 4 != 0)
    {
        return NULL;
    }

    size_t pad = 0;
    if (str[len - 1] == '=')
        pad++;
    if (str[len - 2] == '=')
        pad++;

    uint8_t *out = malloc(len / 4 * 3);
    if (out == NULL)
    {
        handle_oom();
    }

    size_t j = 0;
    for (size_t i = 0; i < len; i += 4)
    {
        uint32_t triple = 0;
        for (size_t k = 0; k < 4; k++)
        {
            int v = 0;
            if (i + k >= len - pad)
            {
                if (str[i + k] != '=')
                    goto invalid;
            }
            else if ((v = base64_value(str[i + k])) < 0)
            {
                goto invalid;
            }
            triple = (triple << 6) | (uint32_t)v;
        }

        out[j++] = (triple >> 16) & 0xFF;
        out[j++] = (triple >> 8) & 0xFF;
        out[j++] = triple & 0xFF;
    }

    *out_len = j - pad;
    return out;

invalid:
    free(out);
    return NULL;
}
