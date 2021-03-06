/* secrets.c
 * Secrets management and processing.
 * Copyright 2018, Peter Wu <peter@lekensteyn.nl>
 *
 * Wireshark - Network traffic analyzer
 * By Gerald Combs <gerald@wireshark.org>
 * Copyright 1998 Gerald Combs
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

/* Start with G_MESSAGES_DEBUG=secrets to see messages. */
#define G_LOG_DOMAIN "secrets"

#include "secrets.h"
#include <wiretap/wtap.h>

#include <string.h>
#ifdef HAVE_LIBGNUTLS
# include <gnutls/gnutls.h>
# include <gnutls/abstract.h>
# include <wsutil/wsgcrypt.h>
# include <wsutil/rsa.h>
# include <epan/uat.h>
# include <wsutil/report_message.h>
# include <wsutil/file_util.h>
# include <errno.h>
#endif  /* HAVE_LIBGNUTLS */

/** Maps guint32 secrets_type -> secrets_block_callback_t. */
static GHashTable *secrets_callbacks;

#ifdef HAVE_LIBGNUTLS
/** Maps public key IDs (cert_key_id_t) -> gnutls_privkey_t.  */
static GHashTable *rsa_privkeys;

typedef struct {
    char *uri;              /**< User-supplied PKCS #11 URI for token or RSA private key file. */
    char *password;         /**< User-supplied PKCS #11 PIN or RSA private key file password. */
} rsa_privkey_record_t;

static uat_t *rsa_privkeys_uat;
static rsa_privkey_record_t *uat_rsa_privkeys;
static guint uat_num_rsa_privkeys;

static void register_rsa_uats(void);
#endif  /* HAVE_LIBGNUTLS */

void
secrets_init(void)
{
    secrets_callbacks = g_hash_table_new(g_direct_hash, g_direct_equal);
#ifdef HAVE_LIBGNUTLS
    rsa_privkeys = privkey_hash_table_new();
    register_rsa_uats();
#endif  /* HAVE_LIBGNUTLS */
}

void
secrets_cleanup(void)
{
    g_hash_table_destroy(secrets_callbacks);
    secrets_callbacks = NULL;
#ifdef HAVE_LIBGNUTLS
    g_hash_table_destroy(rsa_privkeys);
    rsa_privkeys = NULL;
#endif  /* HAVE_LIBGNUTLS */
}

void
secrets_register_type(guint32 secrets_type, secrets_block_callback_t cb)
{
    g_hash_table_insert(secrets_callbacks, GUINT_TO_POINTER(secrets_type), (gpointer)cb);
}

void
secrets_wtap_callback(guint32 secrets_type, const void *secrets, guint size)
{
    secrets_block_callback_t cb = (secrets_block_callback_t)g_hash_table_lookup(
            secrets_callbacks, GUINT_TO_POINTER(secrets_type));
    if (cb) {
        cb(secrets, size);
    }
}

#ifdef HAVE_LIBGNUTLS
static guint
key_id_hash(gconstpointer key)
{
    const cert_key_id_t *key_id = (const cert_key_id_t *)key;
    const guint32 *dw = (const guint32 *)key_id->key_id;

    /* The public key' SHA-1 hash (which maps to a private key) has a uniform
     * distribution, hence simply xor'ing them should be sufficient. */
    return dw[0] ^ dw[1] ^ dw[2] ^ dw[3] ^ dw[4];
}

static gboolean
key_id_equal(gconstpointer a, gconstpointer b)
{
    const cert_key_id_t *key_id_a = (const cert_key_id_t *)a;
    const cert_key_id_t *key_id_b = (const cert_key_id_t *)b;

    return !memcmp(key_id_a, key_id_b, sizeof(*key_id_a));
}

GHashTable *
privkey_hash_table_new(void)
{
    return g_hash_table_new_full(key_id_hash, key_id_equal, g_free, (GDestroyNotify)gnutls_privkey_deinit);
}

static void
rsa_privkey_add(const cert_key_id_t *key_id, gnutls_privkey_t pkey)
{
    void *ht_key = g_memdup(key_id->key_id, sizeof(cert_key_id_t));
    const guint32 *dw = (const guint32 *)key_id->key_id;
    g_hash_table_insert(rsa_privkeys, ht_key, pkey);
    g_debug("Adding key %08x%08x%08x%08x%08x", dw[0], dw[1], dw[2], dw[3], dw[4]);
}

UAT_FILENAME_CB_DEF(rsa_privkeys_uats, uri, rsa_privkey_record_t)
UAT_CSTRING_CB_DEF(rsa_privkeys_uats, password, rsa_privkey_record_t)

static void *
uat_rsa_privkey_copy_str_cb(void *dest, const void *source, size_t len _U_)
{
    rsa_privkey_record_t *d = (rsa_privkey_record_t *)dest;
    const rsa_privkey_record_t *s = (const rsa_privkey_record_t *)source;
    d->uri = g_strdup(s->uri);
    d->password = g_strdup(s->password);
    return dest;
}

static void
uat_rsa_privkey_free_str_cb(void *record)
{
    rsa_privkey_record_t *rec = (rsa_privkey_record_t *)record;
    g_free(rec->uri);
    g_free(rec->password);
}

static void
load_rsa_keyfile(const char *filename, const char *password, char **err)
{
    gnutls_x509_privkey_t x509_priv_key;
    gnutls_privkey_t privkey = NULL;
    char *errmsg = NULL;
    int ret;
    cert_key_id_t key_id;
    size_t size = sizeof(key_id);

    FILE *fp = ws_fopen(filename, "rb");
    if (!fp) {
        *err = g_strdup_printf("Error loading RSA key file %s: %s", filename, g_strerror(errno));
        return;
    }

    if (!password[0]) {
        x509_priv_key = rsa_load_pem_key(fp, &errmsg);
    } else {
        /* Assume encrypted PKCS #12 container. */
        x509_priv_key = rsa_load_pkcs12(fp, password, &errmsg);
    }
    fclose(fp);
    if (!x509_priv_key) {
        *err = g_strdup_printf("Error loading RSA key file %s: %s", filename, errmsg);
        g_free(errmsg);
        return;
    }

    gnutls_privkey_init(&privkey);
    ret = gnutls_privkey_import_x509(privkey, x509_priv_key,
            GNUTLS_PRIVKEY_IMPORT_AUTO_RELEASE|GNUTLS_PRIVKEY_IMPORT_COPY);
    if (ret < 0) {
        *err = g_strdup_printf("Error importing private key %s: %s", filename, gnutls_strerror(ret));
        goto end;
    }
    ret = gnutls_x509_privkey_get_key_id(x509_priv_key, GNUTLS_KEYID_USE_SHA1, key_id.key_id, &size);
    if (ret < 0 || size != sizeof(key_id)) {
        *err = g_strdup_printf("Error calculating Key ID for %s: %s", filename, gnutls_strerror(ret));
        goto end;
    }

    /* Remember the private key. */
    rsa_privkey_add(&key_id, privkey);
    privkey = NULL;

end:
    gnutls_x509_privkey_deinit(x509_priv_key);
    gnutls_privkey_deinit(privkey);
}

static void
uat_rsa_privkeys_post_update(void)
{
    /* Clear previous keys. */
    g_hash_table_remove_all(rsa_privkeys);
    GString *errors = NULL;

    for (guint i = 0; i < uat_num_rsa_privkeys; i++) {
        const rsa_privkey_record_t *rec = &uat_rsa_privkeys[i];
        const char *token_uri = rec->uri;
        char *err = NULL;

        if (g_str_has_prefix(token_uri, "pkcs11:")) {
        } else {
            load_rsa_keyfile(token_uri, rec->password, &err);
        }
        if (err) {
            if (!errors) {
                errors = g_string_new("Error processing rsa_privkeys:");
            }
            g_string_append_c(errors, '\n');
            g_string_append(errors, err);
            g_free(err);
        }
    }
    if (errors) {
        report_failure("%s", errors->str);
        g_string_free(errors, TRUE);
    }
}

/**
 * Register the UAT definitions such that settings can be loaded from file.
 * Note: relies on uat_load_all to invoke the post_update_cb in order of
 * registration below such that libraries are loaded *before* keys are read.
 */
static void
register_rsa_uats(void)
{
    static uat_field_t uat_rsa_privkeys_fields[] = {
        UAT_FLD_FILENAME_OTHER(rsa_privkeys_uats, uri, "Keyfile or Token URI", NULL, "RSA Key File or PKCS #11 URI for token"),
        UAT_FLD_FILENAME_OTHER(rsa_privkeys_uats, password, "Password", NULL, "RSA Key File password or PKCS #11 Token PIN"),
        UAT_END_FIELDS
    };
    rsa_privkeys_uat = uat_new("RSA Private Keys",
            sizeof(rsa_privkey_record_t),
            "rsa_keys",                     /* filename */
            FALSE,                          /* from_profile */
            &uat_rsa_privkeys,              /* data_ptr */
            &uat_num_rsa_privkeys,          /* numitems_ptr */
            0,                              /* does not directly affect dissection */
            NULL,                           /* Help section (currently a wiki page) */
            uat_rsa_privkey_copy_str_cb,    /* copy_cb */
            NULL,                           /* update_cb */
            uat_rsa_privkey_free_str_cb,    /* free_cb */
            uat_rsa_privkeys_post_update,   /* post_update_cb */
            NULL,                           /* reset_cb */
            uat_rsa_privkeys_fields);
}

int
secrets_rsa_decrypt(const cert_key_id_t *key_id, const guint8 *encr, int encr_len, guint8 **out, int *out_len)
{
    gboolean ret;
    gnutls_datum_t ciphertext = { (guchar *)encr, encr_len };
    gnutls_datum_t plain = { 0 };

    gnutls_privkey_t pkey = (gnutls_privkey_t)g_hash_table_lookup(rsa_privkeys, key_id->key_id);
    if (!pkey) {
        return GNUTLS_E_NO_CERTIFICATE_FOUND;
    }

    ret = gnutls_privkey_decrypt_data(pkey, 0, &ciphertext, &plain);
    if (ret == 0) {
        *out = (guint8 *)g_memdup(plain.data, plain.size);
        *out_len = plain.size;
        gnutls_free(plain.data);
    }

    return ret;
}
#endif  /* HAVE_LIBGNUTLS */

/*
 * Editor modelines  -  https://www.wireshark.org/tools/modelines.html
 *
 * Local variables:
 * c-basic-offset: 4
 * tab-width: 8
 * indent-tabs-mode: nil
 * End:
 *
 * vi: set shiftwidth=4 tabstop=8 expandtab:
 * :indentSize=4:tabSize=8:noTabs=true:
 */
