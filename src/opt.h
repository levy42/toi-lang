#ifndef OPT_H
#define OPT_H

#include "chunk.h"

int chunk_instr_length(Chunk* chunk, int offset);
void optimize_chunk(Chunk* chunk);

#endif
