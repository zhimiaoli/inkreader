#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#if defined(__APPLE__)
#include <CommonCrypto/CommonKeyDerivation.h>
#else
int sim_pbkdf2(const unsigned char*,size_t,const unsigned char*,size_t,unsigned,unsigned char*,size_t);
#endif
typedef int psa_status_t;
typedef struct{unsigned iterations;uint8_t salt[16],password[5];size_t salt_len,password_len;} psa_key_derivation_operation_t;
#define PSA_SUCCESS 0
#define PSA_KEY_DERIVATION_OPERATION_INIT {0}
#define PSA_ALG_SHA_256 1
#define PSA_ALG_PBKDF2_HMAC(x) 1
#define PSA_KEY_DERIVATION_INPUT_COST 1
#define PSA_KEY_DERIVATION_INPUT_SALT 2
#define PSA_KEY_DERIVATION_INPUT_PASSWORD 3
static inline int psa_crypto_init(void){return 0;}
static inline int psa_key_derivation_setup(psa_key_derivation_operation_t *p,int a){(void)a;memset(p,0,sizeof(*p));return 0;}
static inline int psa_key_derivation_input_integer(psa_key_derivation_operation_t*p,int key,unsigned n){(void)key;p->iterations=n;return 0;}
static inline int psa_key_derivation_input_bytes(psa_key_derivation_operation_t*p,int key,const uint8_t*b,size_t n){if(key==2){if(n>16)return -1;memcpy(p->salt,b,n);p->salt_len=n;}else{if(n>5)return -1;memcpy(p->password,b,n);p->password_len=n;}return 0;}
static inline int psa_key_derivation_output_bytes(psa_key_derivation_operation_t*p,uint8_t*out,size_t n){
#if defined(__APPLE__)
return CCKeyDerivationPBKDF(kCCPBKDF2,(const char*)p->password,p->password_len,p->salt,p->salt_len,kCCPRFHmacAlgSHA256,p->iterations,out,n);
#else
return sim_pbkdf2(p->password,p->password_len,p->salt,p->salt_len,p->iterations,out,n);
#endif
}
static inline int psa_key_derivation_abort(psa_key_derivation_operation_t*p){memset(p,0,sizeof(*p));return 0;}

/* Offline stubs for hash/MAC/AEAD used by network sync: the simulator has no
 * network, so these calls surface as a generic PSA failure. */
#define PSA_ERROR_GENERIC_ERROR -200
typedef int psa_key_id_t;
typedef struct{int unused;} psa_key_attributes_t;
#define PSA_KEY_ATTRIBUTES_INIT {0}
#define PSA_KEY_TYPE_HMAC 2
#define PSA_KEY_TYPE_AES 3
#define PSA_KEY_USAGE_SIGN_MESSAGE 4
#define PSA_KEY_USAGE_ENCRYPT 8
#define PSA_ALG_GCM 5
#define PSA_ALG_HMAC(x) (x)
static inline int psa_hash_compute(int alg,const uint8_t*in,size_t in_len,uint8_t*out,size_t out_len,size_t*out_len_written){(void)alg;(void)in;(void)in_len;(void)out;(void)out_len;*out_len_written=0;return PSA_ERROR_GENERIC_ERROR;}
static inline void psa_set_key_type(psa_key_attributes_t*a,int t){(void)a;(void)t;}
static inline void psa_set_key_bits(psa_key_attributes_t*a,size_t bits){(void)a;(void)bits;}
static inline void psa_set_key_usage_flags(psa_key_attributes_t*a,int f){(void)a;(void)f;}
static inline void psa_set_key_algorithm(psa_key_attributes_t*a,int alg){(void)a;(void)alg;}
static inline int psa_import_key(const psa_key_attributes_t*a,const uint8_t*data,size_t len,psa_key_id_t*id){(void)a;(void)data;(void)len;*id=0;return PSA_ERROR_GENERIC_ERROR;}
static inline void psa_reset_key_attributes(psa_key_attributes_t*a){(void)a;}
static inline int psa_mac_compute(psa_key_id_t id,int alg,const uint8_t*in,size_t in_len,uint8_t*out,size_t out_len,size_t*out_len_written){(void)id;(void)alg;(void)in;(void)in_len;(void)out;(void)out_len;*out_len_written=0;return PSA_ERROR_GENERIC_ERROR;}
static inline int psa_aead_encrypt(psa_key_id_t id,int alg,const uint8_t*nonce,size_t n_len,const uint8_t*add,size_t add_len,const uint8_t*plain,size_t plain_len,uint8_t*out,size_t out_len,size_t*out_len_written){(void)id;(void)alg;(void)nonce;(void)n_len;(void)add;(void)add_len;(void)plain;(void)plain_len;(void)out;(void)out_len;*out_len_written=0;return PSA_ERROR_GENERIC_ERROR;}
static inline int psa_destroy_key(psa_key_id_t id){(void)id;return 0;}
