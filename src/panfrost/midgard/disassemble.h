#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

void
disassemble_midgard(FILE *fp, uint8_t *code, size_t size, unsigned gpu_id, bool verbose);
