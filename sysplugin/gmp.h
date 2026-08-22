#ifndef NEXUS_GMP_COMPAT_H
#define NEXUS_GMP_COMPAT_H

/* GCC plugin headers require GMP declarations, but the semantic marker plugin does not use GMP. */
#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int _mp_alloc;
    int _mp_size;
    void *_mp_d;
} __mpz_struct;
typedef __mpz_struct mpz_t[1];
typedef __mpz_struct *mpz_ptr;

void __gmpz_init(mpz_ptr);
void __gmpz_clear(mpz_ptr);
#define mpz_init __gmpz_init
#define mpz_clear __gmpz_clear

#ifdef __cplusplus
}
#endif

#endif