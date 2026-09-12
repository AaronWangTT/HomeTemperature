#include <Arduino.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

// The AZ3166 C library does not reliably support floating-point conversion
// through printf-family %f formats. Arduino code already uses dtostrf for this
// purpose, so keeping the same API also fixes String(float) and other callers.

namespace {

char *applyWidth(char *output, signed char width) {
    int requestedWidth = static_cast<int>(width);
    bool leftAligned = requestedWidth < 0;
    if (leftAligned) {
        requestedWidth = -requestedWidth;
    }

    size_t length = strlen(output);
    if (requestedWidth <= static_cast<int>(length)) {
        return output;
    }

    size_t padding = static_cast<size_t>(requestedWidth) - length;
    if (leftAligned) {
        memset(output + length, ' ', padding);
        output[length + padding] = '\0';
    } else {
        memmove(output + padding, output, length + 1);
        memset(output, ' ', padding);
    }
    return output;
}

}  // namespace

// AZ3166 Core 2.0.0 writes zero fractional digits inside its precision loop,
// then appends the remaining value once more. For example, precision 1 formats
// 45.0 as "45.00". This matching C-linkage definition is linked from the sketch
// objects before the Core archive, so it replaces that implementation without
// modifying the installed board package.
extern "C" char *dtostrf(
    double number,
    signed char width,
    unsigned char precision,
    char *output) {
    if (output == NULL) {
        return NULL;
    }
    if (isnan(number)) {
        strcpy(output, "nan");
        return applyWidth(output, width);
    }
    if (isinf(number)) {
        strcpy(output, "inf");
        return applyWidth(output, width);
    }
    if (number > 4294967040.0 || number < -4294967040.0) {
        strcpy(output, "ovf");
        return applyWidth(output, width);
    }

    char *cursor = output;
    if (number < 0.0) {
        *cursor++ = '-';
        number = -number;
    }

    double rounding = 0.5;
    for (unsigned char index = 0; index < precision; ++index) {
        rounding /= 10.0;
    }
    number += rounding;

    unsigned long integerPart = static_cast<unsigned long>(number);
    double remainder = number - static_cast<double>(integerPart);
    cursor += sprintf(cursor, "%lu", integerPart);

    if (precision > 0) {
        *cursor++ = '.';
    }

    for (unsigned char index = 0; index < precision; ++index) {
        remainder *= 10.0;
        unsigned int digit = static_cast<unsigned int>(remainder);
        if (digit > 9) {
            digit = 9;
        }
        *cursor++ = static_cast<char>('0' + digit);
        remainder -= digit;
    }
    *cursor = '\0';

    return applyWidth(output, width);
}