// SPDX-License-Identifier: MIT
#pragma once
#include <stdbool.h>
#include <stdio.h>

/* A complete AT+HELP reply from one byte source. An initial +READY line is
 * allowed; an error, missing OK, or unrelated line makes the capture invalid. */
bool at_test_help_write(FILE *out, const char *transcript);
/* True only when this bare command/verb/synopsis occurs exactly once. */
bool at_test_help_has(const char *transcript, const char *record);
