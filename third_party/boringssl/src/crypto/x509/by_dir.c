/* crypto/x509/by_dir.c */
/* Copyright (C) 1995-1998 Eric Young (eay@cryptsoft.com)
 * All rights reserved.
 *
 * This package is an SSL implementation written
 * by Eric Young (eay@cryptsoft.com).
 * The implementation was written so as to conform with Netscapes SSL.
 *
 * This library is free for commercial and non-commercial use as long as
 * the following conditions are aheared to.  The following conditions
 * apply to all code found in this distribution, be it the RC4, RSA,
 * lhash, DES, etc., code; not just the SSL code.  The SSL documentation
 * included with this distribution is covered by the same copyright terms
 * except that the holder is Tim Hudson (tjh@cryptsoft.com).
 *
 * Copyright remains Eric Young's, and as such any Copyright notices in
 * the code are not to be removed.
 * If this package is used in a product, Eric Young should be given attribution
 * as the author of the parts of the library used.
 * This can be in the form of a textual message at program startup or
 * in documentation (online or textual) provided with the package.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *    "This product includes cryptographic software written by
 *     Eric Young (eay@cryptsoft.com)"
 *    The word 'cryptographic' can be left out if the rouines from the library
 *    being used are not cryptographic related :-).
 * 4. If you include any Windows specific code (or a derivative thereof) from
 *    the apps directory (application code) you must include an acknowledgement:
 *    "This product includes software written by Tim Hudson (tjh@cryptsoft.com)"
 *
 * THIS SOFTWARE IS PROVIDED BY ERIC YOUNG ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * The licence and distribution terms for any publically available version or
 * derivative of this code cannot be changed.  i.e. this code cannot simply be
 * copied and put under another distribution licence
 * [including the GNU Public Licence.] */

#include <openssl/opensslconf.h>
#include <string.h>

#ifndef OPENSSL_NO_POSIX_IO
#include <sys/stat.h>
#endif

#ifdef OPENSSL_SYS_STARBOARD
#include "starboard/configuration_constants.h"
#define LIST_SEPARATOR_CHAR kSbPathSepChar
#endif

#ifdef NATIVE_TARGET_BUILD
#define LIST_SEPARATOR_CHAR ':'
#endif

#ifndef NO_SYS_TYPES_H
#include <sys/types.h>
#endif

#include <openssl/buf.h>
#include <openssl/err.h>
#include <openssl/mem.h>
#include <openssl/thread.h>
#include <openssl/x509.h>

#if !defined(OPENSSL_TRUSTY)

#include "../internal.h"
#include "internal.h"

typedef struct lookup_dir_hashes_st {
    unsigned long hash;
    int suffix;
} BY_DIR_HASH;

typedef struct lookup_dir_entry_st {
    char *dir;
    int dir_type;
    STACK_OF(BY_DIR_HASH) *hashes;
} BY_DIR_ENTRY;

typedef struct lookup_dir_st {
    BUF_MEM *buffer;
    STACK_OF(BY_DIR_ENTRY) *dirs;
} BY_DIR;

DEFINE_STACK_OF(BY_DIR_HASH)
DEFINE_STACK_OF(BY_DIR_ENTRY)

static int dir_ctrl(X509_LOOKUP *ctx, int cmd, const char *argp, long argl,
                    char **ret);
static int new_dir(X509_LOOKUP *lu);
static void free_dir(X509_LOOKUP *lu);
static int add_cert_dir(BY_DIR *ctx, const char *dir, int type);
static int get_cert_by_subject(X509_LOOKUP *xl, int type, X509_NAME *name,
                               X509_OBJECT *ret);
static X509_LOOKUP_METHOD x509_dir_lookup = {
    "Load certs from files in a directory",
    new_dir,                    /* new */
    free_dir,                   /* free */
    NULL,                       /* init */
    NULL,                       /* shutdown */
    dir_ctrl,                   /* ctrl */
    get_cert_by_subject,        /* get_by_subject */
    NULL,                       /* get_by_issuer_serial */
    NULL,                       /* get_by_fingerprint */
    NULL,                       /* get_by_alias */
};

X509_LOOKUP_METHOD *X509_LOOKUP_hash_dir(void)
{
    return (&x509_dir_lookup);
}

static int dir_ctrl(X509_LOOKUP *ctx, int cmd, const char *argp, long argl,
                    char **retp)
{
    int ret = 0;
    BY_DIR *ld;
    char *dir = NULL;

    ld = (BY_DIR *)ctx->method_data;

    switch (cmd) {
    case X509_L_ADD_DIR:
        if (argl == X509_FILETYPE_DEFAULT) {
#if defined(COBALT)
            // We don't expect to use the default certs dir.
            OPENSSL_PUT_ERROR(X509, X509_R_LOADING_CERT_DIR);
#else
            dir = (char *)OPENSSL_port_getenv(X509_get_default_cert_dir_env());
            if (dir)
                ret = add_cert_dir(ld, dir, X509_FILETYPE_PEM);
            else
                ret = add_cert_dir(ld, X509_get_default_cert_dir(),
                                   X509_FILETYPE_PEM);
            if (!ret) {
                OPENSSL_PUT_ERROR(X509, X509_R_LOADING_CERT_DIR);
            }
#endif
        } else
            ret = add_cert_dir(ld, argp, (int)argl);
        break;
    }
    return (ret);
}

static int new_dir(X509_LOOKUP *lu)
{
    BY_DIR *a;

    if ((a = (BY_DIR *)OPENSSL_malloc(sizeof(BY_DIR))) == NULL)
        return (0);
    if ((a->buffer = BUF_MEM_new()) == NULL) {
        OPENSSL_free(a);
        return (0);
    }
    a->dirs = NULL;
    lu->method_data = (char *)a;
    return (1);
}

static void by_dir_hash_free(BY_DIR_HASH *hash)
{
    OPENSSL_free(hash);
}

static int by_dir_hash_cmp(const BY_DIR_HASH **a, const BY_DIR_HASH **b)
{
    if ((*a)->hash > (*b)->hash)
        return 1;
    if ((*a)->hash < (*b)->hash)
        return -1;
    return 0;
}

static void by_dir_entry_free(BY_DIR_ENTRY *ent)
{
    if (ent->dir)
        OPENSSL_free(ent->dir);
    if (ent->hashes)
        sk_BY_DIR_HASH_pop_free(ent->hashes, by_dir_hash_free);
    OPENSSL_free(ent);
}

static void free_dir(X509_LOOKUP *lu)
{
    BY_DIR *a;

    a = (BY_DIR *)lu->method_data;
    if (a->dirs != NULL)
        sk_BY_DIR_ENTRY_pop_free(a->dirs, by_dir_entry_free);
    if (a->buffer != NULL)
        BUF_MEM_free(a->buffer);
    OPENSSL_free(a);
}

static int add_cert_dir(BY_DIR *ctx, const char *dir, int type)
{
    size_t j, len;
    const char *s, *ss, *p;

    if (dir == NULL || !*dir) {
        OPENSSL_PUT_ERROR(X509, X509_R_INVALID_DIRECTORY);
        return 0;
    }

    s = dir;
    p = s;
    do {
      if ((*p == LIST_SEPARATOR_CHAR) || (*p == '\0')) {
            BY_DIR_ENTRY *ent;
            ss = s;
            s = p + 1;
            len = p - ss;
            if (len == 0)
                continue;
            for (j = 0; j < sk_BY_DIR_ENTRY_num(ctx->dirs); j++) {
                ent = sk_BY_DIR_ENTRY_value(ctx->dirs, j);
                if (strlen(ent->dir) == len &&
                    strncmp(ent->dir, ss, len) == 0)
                    break;
            }
            if (j < sk_BY_DIR_ENTRY_num(ctx->dirs))
                continue;
            if (ctx->dirs == NULL) {
                ctx->dirs = sk_BY_DIR_ENTRY_new_null();
                if (!ctx->dirs) {
                    OPENSSL_PUT_ERROR(X509, ERR_R_MALLOC_FAILURE);
                    return 0;
                }
            }
            ent = OPENSSL_malloc(sizeof(BY_DIR_ENTRY));
            if (!ent)
                return 0;
            ent->dir_type = type;
            ent->hashes = sk_BY_DIR_HASH_new(by_dir_hash_cmp);
            ent->dir = OPENSSL_malloc(len + 1);
            if (!ent->dir || !ent->hashes) {
                by_dir_entry_free(ent);
                return 0;
            }
            OPENSSL_strlcpy(ent->dir, ss, len + 1);
            if (!sk_BY_DIR_ENTRY_push(ctx->dirs, ent)) {
                by_dir_entry_free(ent);
                return 0;
            }
        }
    } while (*p++ != '\0');
    return 1;
}

/*
 * g_ent_hashes_lock protects the |hashes| member of all |BY_DIR_ENTRY|
 * objects.
 */
static struct CRYPTO_STATIC_MUTEX g_ent_hashes_lock =
    CRYPTO_STATIC_MUTEX_INIT;

static int get_cert_by_subject(X509_LOOKUP *xl, int type, X509_NAME *name,
                               X509_OBJECT *ret)
{
    BY_DIR *ctx;
    union {
        struct {
            X509 st_x509;
            X509_CINF st_x509_cinf;
        } x509;
        struct {
            X509_CRL st_crl;
            X509_CRL_INFO st_crl_info;
        } crl;
    } data;
    int ok = 0;
    size_t i;
    int j, k;
    unsigned long h;
    unsigned long hash_array[2];
    int hash_index;
    BUF_MEM *b = NULL;
    X509_OBJECT stmp, *tmp;
    const char *postfix = "";

    if (name == NULL)
        return (0);

    stmp.type = type;
    if (type == X509_LU_X509) {
        data.x509.st_x509.cert_info = &data.x509.st_x509_cinf;
        data.x509.st_x509_cinf.subject = name;
        stmp.data.x509 = &data.x509.st_x509;
        postfix = "";
    } else if (type == X509_LU_CRL) {
        data.crl.st_crl.crl = &data.crl.st_crl_info;
        data.crl.st_crl_info.issuer = name;
        stmp.data.crl = &data.crl.st_crl;
        postfix = "r";
    } else {
        OPENSSL_PUT_ERROR(X509, X509_R_WRONG_LOOKUP_TYPE);
        goto finish;
    }

    if ((b = BUF_MEM_new()) == NULL) {
        OPENSSL_PUT_ERROR(X509, ERR_R_BUF_LIB);
        goto finish;
    }

    ctx = (BY_DIR *)xl->method_data;

    hash_array[0] = X509_NAME_hash(name);
    hash_array[1] = X509_NAME_hash_old(name);
    for (hash_index = 0; hash_index < 2; ++hash_index) {
        h = hash_array[hash_index];
        for (i = 0; i < sk_BY_DIR_ENTRY_num(ctx->dirs); i++) {
            BY_DIR_ENTRY *ent;
            size_t idx;
            BY_DIR_HASH htmp, *hent;
            ent = sk_BY_DIR_ENTRY_value(ctx->dirs, i);
            j = strlen(ent->dir) + 1 + 8 + 6 + 1 + 1;
            if (!BUF_MEM_grow(b, j)) {
                OPENSSL_PUT_ERROR(X509, ERR_R_MALLOC_FAILURE);
                goto finish;
            }
            if (type == X509_LU_CRL && ent->hashes) {
                htmp.hash = h;
                CRYPTO_STATIC_MUTEX_lock_read(&g_ent_hashes_lock);
                if (sk_BY_DIR_HASH_find(ent->hashes, &idx, &htmp)) {
                    hent = sk_BY_DIR_HASH_value(ent->hashes, idx);
                    k = hent->suffix;
                } else {
                    hent = NULL;
                    k = 0;
                }
                CRYPTO_STATIC_MUTEX_unlock_read(&g_ent_hashes_lock);
            } else {
                k = 0;
                hent = NULL;
            }
            for (;;) {
                char c = '/';
#ifdef OPENSSL_SYS_VMS
                c = ent->dir[strlen(ent->dir) - 1];
                if (c != ':' && c != '>' && c != ']') {
                    /*
                     * If no separator is present, we assume the directory
                     * specifier is a logical name, and add a colon.  We
                     * really should use better VMS routines for merging
                     * things like this, but this will do for now... --
                     * Richard Levitte
                     */
                    c = ':';
                } else {
                    c = '\0';
                }
#endif
                if (c == '\0') {
                    /*
                     * This is special.  When c == '\0', no directory
                     * separator should be added.
                     */
                    BIO_snprintf(b->data, b->max,
                                 "%s%08lx.%s%d", ent->dir, h, postfix, k);
                } else {
                    BIO_snprintf(b->data, b->max,
                                 "%s%c%08lx.%s%d", ent->dir, c, h,
                                 postfix, k);
                }
#ifndef OPENSSL_NO_POSIX_IO
# if defined(_WIN32) && !defined(stat)
#  define stat _stat
# endif
                {
                    struct stat st;
                    if (stat(b->data, &st) < 0)
                        break;
                }
#endif
                /* found one. */
                if (type == X509_LU_X509) {
                    if ((X509_load_cert_file(xl, b->data,
                                             ent->dir_type)) == 0)
                        break;
                } else if (type == X509_LU_CRL) {
                    if ((X509_load_crl_file(xl, b->data, ent->dir_type)) == 0)
                        break;
                }
                /* else case will caught higher up */
                k++;
            }

            /*
             * we have added it to the cache so now pull it out again
             */
            CRYPTO_MUTEX_lock_write(&xl->store_ctx->objs_lock);
            tmp = NULL;
            sk_X509_OBJECT_sort(xl->store_ctx->objs);
            if (sk_X509_OBJECT_find(xl->store_ctx->objs, &idx, &stmp)) {
                tmp = sk_X509_OBJECT_value(xl->store_ctx->objs, idx);
            }
            CRYPTO_MUTEX_unlock_write(&xl->store_ctx->objs_lock);

            /*
             * If a CRL, update the last file suffix added for this
             */

            if (type == X509_LU_CRL) {
                CRYPTO_STATIC_MUTEX_lock_write(&g_ent_hashes_lock);
                /*
                 * Look for entry again in case another thread added an entry
                 * first.
                 */
                if (!hent) {
                    htmp.hash = h;
                    sk_BY_DIR_HASH_sort(ent->hashes);
                    if (sk_BY_DIR_HASH_find(ent->hashes, &idx, &htmp))
                        hent = sk_BY_DIR_HASH_value(ent->hashes, idx);
                }
                if (!hent) {
                    hent = OPENSSL_malloc(sizeof(BY_DIR_HASH));
                    if (hent == NULL) {
                        CRYPTO_STATIC_MUTEX_unlock_write(&g_ent_hashes_lock);
                        ok = 0;
                        goto finish;
                    }
                    hent->hash = h;
                    hent->suffix = k;
                    if (!sk_BY_DIR_HASH_push(ent->hashes, hent)) {
                        CRYPTO_STATIC_MUTEX_unlock_write(&g_ent_hashes_lock);
                        OPENSSL_free(hent);
                        ok = 0;
                        goto finish;
                    }
                    sk_BY_DIR_HASH_sort(ent->hashes);
                } else if (hent->suffix < k)
                    hent->suffix = k;

                CRYPTO_STATIC_MUTEX_unlock_write(&g_ent_hashes_lock);
            }

            if (tmp != NULL) {
                ok = 1;
                ret->type = tmp->type;
                OPENSSL_memcpy(&ret->data, &tmp->data, sizeof(ret->data));

                /*
                 * Clear any errors that might have been raised processing empty
                 * or malformed files.
                 */
                ERR_clear_error();

                /*
                 * If we were going to up the reference count, we would need
                 * to do it on a perl 'type' basis
                 */
                /*
                 * CRYPTO_add(&tmp->data.x509->references,1,
                 * CRYPTO_LOCK_X509);
                 */
                goto finish;
            }
        }
    }
 finish:
    if (b != NULL)
        BUF_MEM_free(b);
    return (ok);
}

#endif  // OPENSSL_TRUSTY
