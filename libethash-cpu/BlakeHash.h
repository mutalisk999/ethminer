#ifndef __BLAKEHASH_H__
#define __BLAKEHASH_H__

#ifdef __cplusplus
extern "C" {
#endif

void decred_hash(char *state, const char *input, unsigned int len);
void sia_hash(char *output, const char *input, unsigned int len);

#ifdef __cplusplus
}
#endif

#endif
