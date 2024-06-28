/*
 * Copyright (C) 2024 wolfSSL Inc.
 *
 * This file is part of wolfHSM.
 *
 * wolfHSM is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * wolfHSM is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with wolfHSM.  If not, see <http://www.gnu.org/licenses/>.
 */
/*
 * src/wh_server_she.c
 *
 */

#ifdef WOLFHSM_SHE_EXTENSION

/* System libraries */
#include <stdint.h>
#include <stdlib.h>  /* For NULL */
#include <string.h>  /* For memset, memcpy */

#ifndef WOLFHSM_NO_CRYPTO
#include "wolfssl/wolfcrypt/settings.h"
#include "wolfssl/wolfcrypt/types.h"
#include "wolfssl/wolfcrypt/error-crypt.h"
#include "wolfssl/wolfcrypt/wc_port.h"
#include "wolfssl/wolfcrypt/aes.h"
#include "wolfssl/wolfcrypt/cmac.h"
#endif /* !WOLFHSM_NO_CRYPTO */

#include "wolfhsm/wh_server.h"
#include "wolfhsm/wh_server_keystore.h"
#include "wolfhsm/wh_message.h"
#include "wolfhsm/wh_packet.h"
#include "wolfhsm/wh_error.h"
#include "wolfhsm/wh_utils.h"
#include "wolfhsm/wh_she_common.h"

#include "wolfhsm/wh_server_she.h"

#ifndef WOLFHSM_NO_CRYPTO

/** SHE defined constants */
static const uint8_t WOLFHSM_SHE_KEY_UPDATE_ENC_C[] = { 0x01, 0x01, 0x53, 0x48,
        0x45, 0x00, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xB0};
static const uint8_t WOLFHSM_SHE_KEY_UPDATE_MAC_C[] = { 0x01, 0x02, 0x53, 0x48,
        0x45, 0x00, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xB0};
static const uint8_t WOLFHSM_SHE_PRNG_KEY_C[]       = { 0x01, 0x04, 0x53, 0x48,
        0x45, 0x00, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xB0};
static const uint8_t WOLFHSM_SHE_PRNG_SEED_KEY_C[]  = { 0x01, 0x05, 0x53, 0x48,
        0x45, 0x00, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xB0};

enum WOLFHSM_SHE_SB_STATE {
    WOLFHSM_SHE_SB_INIT,
    WOLFHSM_SHE_SB_UPDATE,
    WOLFHSM_SHE_SB_FINISH,
    WOLFHSM_SHE_SB_SUCCESS,
    WOLFHSM_SHE_SB_FAILURE,
};
/* cmac is global since the bootloader update can be called multiple times */

/** Local Declarations */
static int wh_AesMp16(whServerContext* server, uint8_t* in, word32 inSz,
        uint8_t* out);
static uint16_t hsmShePopAuthId(uint8_t* messageOne);
static uint32_t hsmShePopFlags(uint8_t* messageTwo);
static int hsmSheSetUid(whServerContext* server, whPacket* packet);
static int hsmSheSecureBootInit(whServerContext* server, whPacket* packet,
        uint16_t* size);
static int hsmSheSecureBootUpdate(whServerContext* server, whPacket* packet,
    uint16_t* size);
static int hsmSheSecureBootFinish(whServerContext* server, whPacket* packet,
    uint16_t* size);
static int hsmSheGetStatus(whServerContext* server, whPacket* packet,
    uint16_t* size);
static int hsmSheLoadKey(whServerContext* server, whPacket* packet,
    uint16_t* size);
static int hsmSheLoadPlainKey(whServerContext* server, whPacket* packet,
    uint16_t* size);
static int hsmSheExportRamKey(whServerContext* server, whPacket* packet,
    uint16_t* size);
static int hsmSheRnd(whServerContext* server, whPacket* packet, uint16_t* size);
static int hsmSheExtendSeed(whServerContext* server, whPacket* packet,
    uint16_t* size);
static int hsmSheEncEcb(whServerContext* server, whPacket* packet,
        uint16_t* size);
static int hsmSheEncCbc(whServerContext* server, whPacket* packet,
    uint16_t* size);
static int hsmSheDecEcb(whServerContext* server, whPacket* packet,
    uint16_t* size);
static int hsmSheDecCbc(whServerContext* server, whPacket* packet,
    uint16_t* size);
static int hsmSheGenerateMac(whServerContext* server, whPacket* packet,
    uint16_t* size);


/** Local Implementations */

/* kdf function based on the Miyaguchi-Preneel one-way compression function */
static int wh_AesMp16(whServerContext* server, uint8_t* in, word32 inSz,
        uint8_t* out)
{
    int ret;
    int i = 0;
    int j;
    uint8_t paddedInput[AES_BLOCK_SIZE];
    uint8_t messageZero[WOLFHSM_SHE_KEY_SZ] = {0};
    /* check valid inputs */
    if (server == NULL || in == NULL || inSz == 0 || out == NULL)
        return WH_ERROR_BADARGS;
    /* init with hw */
    ret = wc_AesInit(server->she->sheAes, NULL, server->crypto->devId);
    /* do the first block with messageZero as the key */
    if (ret == 0) {
        ret = wc_AesSetKeyDirect(server->she->sheAes, messageZero,
            AES_BLOCK_SIZE, NULL, AES_ENCRYPTION);
    }
    while (ret == 0 && i < (int)inSz) {
        /* copy a block and pad it if we're short */
        if ((int)inSz - i < (int)AES_BLOCK_SIZE) {
            XMEMCPY(paddedInput, in + i, inSz - i);
            XMEMSET(paddedInput + inSz - i, 0, AES_BLOCK_SIZE - (inSz - i));
        }
        else
            XMEMCPY(paddedInput, in + i, AES_BLOCK_SIZE);
        /* encrypt this block */
        ret = wc_AesEncryptDirect(server->she->sheAes, out, paddedInput);
        /* xor with the original message and then the previous block */
        for (j = 0; j < (int)AES_BLOCK_SIZE; j++) {
            out[j] ^= paddedInput[j];
            /* use messageZero as our previous output buffer */
            out[j] ^= messageZero[j];
        }
        /* set the key for the next block */
        if (ret == 0) {
            ret = wc_AesSetKeyDirect(server->she->sheAes, out, AES_BLOCK_SIZE,
                NULL, AES_ENCRYPTION);
        }
        if (ret == 0) {
            /* store previous output in messageZero */
            XMEMCPY(messageZero, out, AES_BLOCK_SIZE);
            /* increment to next block */
            i += AES_BLOCK_SIZE;
        }
    }
    /* free aes for protection */
    wc_AesFree(server->she->sheAes);
    return ret;
}

/* AuthID is the 4 rightmost bits of messageOne */
static uint16_t hsmShePopAuthId(uint8_t* messageOne)
{
    return (*(messageOne + WOLFHSM_SHE_M1_SZ - 1) & 0x0f);
}

/* ID is the second to last 4 bits of messageOne */
static uint16_t hsmShePopId(uint8_t* messageOne)
{
    return ((*(messageOne + WOLFHSM_SHE_M1_SZ - 1) & 0xf0) >> 4);
}

/* flags are the rightmost 4 bits of byte 3 as it's leftmost bits
 * and leftmost bit of byte 4 as it's rightmost bit */
static uint32_t hsmShePopFlags(uint8_t* messageTwo)
{
    return (((messageTwo[3] & 0x0f) << 4) | ((messageTwo[4] & 0x80) >> 7));
}

static int hsmSheSetUid(whServerContext* server, whPacket* packet)
{
    int ret = 0;
    if (server->she->uidSet == 1)
        ret = WH_SHE_ERC_SEQUENCE_ERROR;
    if (ret == 0) {
        XMEMCPY(server->she->uid, packet->sheSetUidReq.uid,
            sizeof(packet->sheSetUidReq.uid));
        server->she->uidSet = 1;
    }
    return ret;
}

static int hsmSheSecureBootInit(whServerContext* server, whPacket* packet,
    uint16_t* size)
{
    int ret = 0;
    uint32_t keySz;
    uint8_t macKey[WOLFHSM_SHE_KEY_SZ];
    /* if we aren't looking for init return error */
    if (server->she->sbState != WOLFHSM_SHE_SB_INIT)
        ret = WH_SHE_ERC_SEQUENCE_ERROR;
    if (ret == 0) {
        /* set the expected size */
        server->she->blSize = packet->sheSecureBootInitReq.sz;
        /* check if the boot mac key is empty */
        keySz = sizeof(macKey);
        ret = hsmReadKey(server, MAKE_WOLFHSM_KEYID(WOLFHSM_KEYTYPE_SHE,
            server->comm->client_id, WOLFHSM_SHE_BOOT_MAC_KEY_ID), NULL,
            macKey, &keySz);
        /* if the key wasn't found */
        if (ret != 0) {
            /* return ERC_NO_SECURE_BOOT */
            ret = WH_SHE_ERC_NO_SECURE_BOOT;
            /* skip SB process since we have no key */
            server->she->sbState = WOLFHSM_SHE_SB_SUCCESS;
            server->she->cmacKeyFound = 0;
        }
        else
            server->she->cmacKeyFound = 1;
    }
    /* init the cmac, use const length since the nvm key holds both key and
     * expected digest so meta->len will be too long */
    if (ret == 0) {
        ret = wc_InitCmac_ex(server->she->sheCmac, macKey, WOLFHSM_SHE_KEY_SZ,
            WC_CMAC_AES, NULL, NULL, server->crypto->devId);
    }
    /* hash 12 zeros */
    if (ret == 0) {
        XMEMSET(macKey, 0, WOLFHSM_SHE_BOOT_MAC_PREFIX_LEN);
        ret = wc_CmacUpdate(server->she->sheCmac, macKey, WOLFHSM_SHE_BOOT_MAC_PREFIX_LEN);
    }
    /* TODO is size big or little endian? spec says it is 32 bit */
    /* hash size */
    if (ret == 0) {
        ret = wc_CmacUpdate(server->she->sheCmac, (uint8_t*)&server->she->blSize,
            sizeof(server->she->blSize));
    }
    if (ret == 0) {
        /* advance to the next state */
        server->she->sbState = WOLFHSM_SHE_SB_UPDATE;
        /* set ERC_NO_ERROR */
        packet->sheSecureBootInitRes.status = WOLFHSM_SHE_ERC_NO_ERROR;
        *size = WOLFHSM_PACKET_STUB_SIZE +
            sizeof(packet->sheSecureBootInitRes);
    }
    return ret;
}

static int hsmSheSecureBootUpdate(whServerContext* server, whPacket* packet,
    uint16_t* size)
{
    int ret = 0;
    uint8_t* in;
    /* if we aren't looking for update return error */
    if (server->she->sbState != WOLFHSM_SHE_SB_UPDATE)
        ret = WH_SHE_ERC_SEQUENCE_ERROR;
    if (ret == 0) {
        /* the bootloader chunk is after the fixed fields */
        in = (uint8_t*)(&packet->sheSecureBootUpdateReq + 1);
        /* increment blSizeReceived */
        server->she->blSizeReceived += packet->sheSecureBootUpdateReq.sz;
        /* check that we didn't exceed the expected bootloader size */
        if (server->she->blSizeReceived > server->she->blSize)
            ret = WH_SHE_ERC_SEQUENCE_ERROR;
    }
    /* update with the new input */
    if (ret == 0)
        ret = wc_CmacUpdate(server->she->sheCmac, in, packet->sheSecureBootUpdateReq.sz);
    if (ret == 0) {
        /* advance to the next state if we've cmaced the entire image */
        if (server->she->blSizeReceived == server->she->blSize)
            server->she->sbState = WOLFHSM_SHE_SB_FINISH;
        /* set ERC_NO_ERROR */
        packet->sheSecureBootUpdateRes.status = WOLFHSM_SHE_ERC_NO_ERROR;
        *size = WOLFHSM_PACKET_STUB_SIZE +
            sizeof(packet->sheSecureBootUpdateRes);
    }
    return ret;
}

static int hsmSheSecureBootFinish(whServerContext* server, whPacket* packet,
    uint16_t* size)
{
    int ret = 0;
    uint32_t keySz;
    uint32_t field;
    uint8_t cmacOutput[AES_BLOCK_SIZE];
    uint8_t macDigest[WOLFHSM_SHE_KEY_SZ];
    /* if we aren't looking for finish return error */
    if (server->she->sbState != WOLFHSM_SHE_SB_FINISH)
        ret = WH_SHE_ERC_SEQUENCE_ERROR;
    /* call final */
    if (ret == 0) {
        field = AES_BLOCK_SIZE;
        ret = wc_CmacFinal(server->she->sheCmac, cmacOutput, (word32*)&field);
    }
    /* load the cmac to check */
    if (ret == 0) {
        keySz = sizeof(macDigest);
        ret = hsmReadKey(server, MAKE_WOLFHSM_KEYID(WOLFHSM_KEYTYPE_SHE,
            server->comm->client_id, WOLFHSM_SHE_BOOT_MAC), NULL, macDigest,
            &keySz);
        if (ret != 0)
            ret = WH_SHE_ERC_KEY_NOT_AVAILABLE;
    }
    if (ret == 0) {
        /* compare and set either success or failure */
        ret = XMEMCMP(cmacOutput, macDigest, field);
        if (ret == 0) {
            server->she->sbState = WOLFHSM_SHE_SB_SUCCESS;
            packet->sheSecureBootFinishRes.status = WOLFHSM_SHE_ERC_NO_ERROR;
            *size = WOLFHSM_PACKET_STUB_SIZE +
                sizeof(packet->sheSecureBootFinishRes);
        }
        else {
            server->she->sbState = WOLFHSM_SHE_SB_FAILURE;
            ret = WH_SHE_ERC_GENERAL_ERROR;
        }
    }
    return ret;
}

static int hsmSheGetStatus(whServerContext* server, whPacket* packet,
    uint16_t* size)
{
    /* TODO do we care about all the sreg fields? */
    packet->sheGetStatusRes.sreg = 0;
    /* SECURE_BOOT */
    if (server->she->cmacKeyFound)
        packet->sheGetStatusRes.sreg |= WOLFHSM_SHE_SREG_SECURE_BOOT;
    /* BOOT_FINISHED */
    if (server->she->sbState == WOLFHSM_SHE_SB_SUCCESS ||
        server->she->sbState == WOLFHSM_SHE_SB_FAILURE) {
        packet->sheGetStatusRes.sreg |= WOLFHSM_SHE_SREG_BOOT_FINISHED;
    }
    /* BOOT_OK */
    if (server->she->sbState == WOLFHSM_SHE_SB_SUCCESS)
        packet->sheGetStatusRes.sreg |= WOLFHSM_SHE_SREG_BOOT_OK;
    /* RND_INIT */
    if (server->she->rndInited == 1)
        packet->sheGetStatusRes.sreg |= WOLFHSM_SHE_SREG_RND_INIT;
    *size = WOLFHSM_PACKET_STUB_SIZE + sizeof(packet->sheGetStatusRes);
    return 0;
}

static int hsmSheLoadKey(whServerContext* server, whPacket* packet,
    uint16_t* size)
{
    int ret;
    int keyRet = 0;
    uint32_t keySz;
    uint32_t field;
    uint8_t kdfInput[WOLFHSM_SHE_KEY_SZ * 2];
    uint8_t cmacOutput[AES_BLOCK_SIZE];
    uint8_t tmpKey[WOLFHSM_SHE_KEY_SZ];
    whNvmMetadata meta[1] = {{0}};
    whSheMetadata* she_meta = (whSheMetadata*)meta->label;
    uint32_t* counter;

    /* read the auth key by AuthID */
    keySz = sizeof(kdfInput);
    ret = hsmReadKey(server, MAKE_WOLFHSM_KEYID(WOLFHSM_KEYTYPE_SHE,
        server->comm->client_id,
        hsmShePopAuthId(packet->sheLoadKeyReq.messageOne)), NULL, kdfInput,
        &keySz);
    /* make K2 using AES-MP(authKey | WOLFHSM_SHE_KEY_UPDATE_MAC_C) */
    if (ret == 0) {
        /* add WOLFHSM_SHE_KEY_UPDATE_MAC_C to the input */
        XMEMCPY(kdfInput + keySz, WOLFHSM_SHE_KEY_UPDATE_MAC_C,
            sizeof(WOLFHSM_SHE_KEY_UPDATE_MAC_C));
        /* do kdf */
        ret = wh_AesMp16(server, kdfInput,
            keySz + sizeof(WOLFHSM_SHE_KEY_UPDATE_MAC_C), tmpKey);
    }
    else
        ret = WH_SHE_ERC_KEY_NOT_AVAILABLE;
    /* cmac messageOne and messageTwo using K2 as the cmac key */
    if (ret == 0) {
        field = AES_BLOCK_SIZE;
        ret = wc_AesCmacGenerate_ex(server->she->sheCmac, cmacOutput, (word32*)&field,
            (uint8_t*)&packet->sheLoadKeyReq,
            sizeof(packet->sheLoadKeyReq.messageOne) +
            sizeof(packet->sheLoadKeyReq.messageTwo), tmpKey,
            WOLFHSM_SHE_KEY_SZ, NULL, server->crypto->devId);
    }
    /* compare digest to M3 */
    if (ret == 0 && XMEMCMP(packet->sheLoadKeyReq.messageThree,
        cmacOutput, field) != 0) {
        ret = WH_SHE_ERC_KEY_UPDATE_ERROR;
    }
    /* make K1 using AES-MP(authKey | WOLFHSM_SHE_KEY_UPDATE_ENC_C) */
    if (ret == 0) {
        /* add WOLFHSM_SHE_KEY_UPDATE_ENC_C to the input */
        XMEMCPY(kdfInput + keySz, WOLFHSM_SHE_KEY_UPDATE_ENC_C,
            sizeof(WOLFHSM_SHE_KEY_UPDATE_ENC_C));
        /* do kdf */
        ret = wh_AesMp16(server, kdfInput,
            keySz + sizeof(WOLFHSM_SHE_KEY_UPDATE_ENC_C), tmpKey);
    }
    /* decrypt messageTwo */
    if (ret == 0)
        ret = wc_AesInit(server->she->sheAes, NULL, server->crypto->devId);
    if (ret == 0) {
        ret = wc_AesSetKey(server->she->sheAes, tmpKey, WOLFHSM_SHE_KEY_SZ,
            NULL, AES_DECRYPTION);
    }
    if (ret == 0) {
        ret = wc_AesCbcDecrypt(server->she->sheAes,
            packet->sheLoadKeyReq.messageTwo,
            packet->sheLoadKeyReq.messageTwo,
            sizeof(packet->sheLoadKeyReq.messageTwo));
    }
    /* free aes for protection */
    wc_AesFree(server->she->sheAes);
    /* load the target key */
    if (ret == 0) {
        ret = hsmReadKey(server, MAKE_WOLFHSM_KEYID(WOLFHSM_KEYTYPE_SHE,
            server->comm->client_id,
            hsmShePopId(packet->sheLoadKeyReq.messageOne)), meta, kdfInput,
            &keySz);
        /* if the keyslot is empty or write protection is not on continue */
        if (ret == WH_ERROR_NOTFOUND ||
            (she_meta->flags & WOLFHSM_SHE_FLAG_WRITE_PROTECT) == 0) {
            keyRet = ret;
            ret = 0;
        }
        else
            ret = WH_SHE_ERC_WRITE_PROTECTED;
    }
    /* check UID == 0 */
    if (ret == 0 && wh_Utils_memeqzero(packet->sheLoadKeyReq.messageOne,
        WOLFHSM_SHE_UID_SZ) == 1) {
        /* check wildcard */
        if ((she_meta->flags & WOLFHSM_SHE_FLAG_WILDCARD) == 0) {
            ret = WH_SHE_ERC_KEY_UPDATE_ERROR;
        }
    }
    /* compare to UID */
    else if (ret == 0 && XMEMCMP(packet->sheLoadKeyReq.messageOne,
        server->she->uid, sizeof(server->she->uid)) != 0) {
        ret = WH_SHE_ERC_KEY_UPDATE_ERROR;
    }
    /* verify counter is greater than stored value */
    counter = (uint32_t*)packet->sheLoadKeyReq.messageTwo;
    if (ret == 0 &&
        keyRet != WH_ERROR_NOTFOUND &&
        wh_Utils_ntohl(*counter >> 4) <= wh_Utils_ntohl(she_meta->count)) {
        ret = WH_SHE_ERC_KEY_UPDATE_ERROR;
    }
    /* write key with counter */
    if (ret == 0) {
        meta->id = MAKE_WOLFHSM_KEYID(WOLFHSM_KEYTYPE_SHE,
            server->comm->client_id,
            hsmShePopId(packet->sheLoadKeyReq.messageOne));
        she_meta->flags =
            hsmShePopFlags(packet->sheLoadKeyReq.messageTwo);
        she_meta->count = (*counter >> 4);
        meta->len = WOLFHSM_SHE_KEY_SZ;
        /* cache if ram key, overwrite otherwise */
        if ((meta->id & WOLFHSM_KEYID_MASK) == WOLFHSM_SHE_RAM_KEY_ID) {
            ret = hsmCacheKey(server, meta, packet->sheLoadKeyReq.messageTwo
                + WOLFHSM_SHE_KEY_SZ);
        }
        else {
            ret = wh_Nvm_AddObject(server->nvm, meta, meta->len,
                packet->sheLoadKeyReq.messageTwo + WOLFHSM_SHE_KEY_SZ);
            /* read the evicted back from nvm */
            if (ret == 0) {
                keySz = WOLFHSM_SHE_KEY_SZ;
                ret = hsmReadKey(server, meta->id, meta,
                    packet->sheLoadKeyReq.messageTwo + WOLFHSM_SHE_KEY_SZ,
                    &keySz);
            }
        }
        if (ret != 0)
            ret = WH_SHE_ERC_KEY_UPDATE_ERROR;
    }
    /* generate K3 using the updated key */
    if (ret == 0) {
        /* copy new key to kdfInput */
        XMEMCPY(kdfInput, packet->sheLoadKeyReq.messageTwo +
            WOLFHSM_SHE_KEY_SZ, WOLFHSM_SHE_KEY_SZ);
        /* add WOLFHSM_SHE_KEY_UPDATE_ENC_C to the input */
        XMEMCPY(kdfInput + meta->len, WOLFHSM_SHE_KEY_UPDATE_ENC_C,
            sizeof(WOLFHSM_SHE_KEY_UPDATE_ENC_C));
        /* do kdf */
        ret = wh_AesMp16(server, kdfInput,
            meta->len + sizeof(WOLFHSM_SHE_KEY_UPDATE_ENC_C), tmpKey);
    }
    if (ret == 0)
        ret = wc_AesInit(server->she->sheAes, NULL, server->crypto->devId);
    if (ret == 0) {
        ret = wc_AesSetKey(server->she->sheAes, tmpKey, WOLFHSM_SHE_KEY_SZ,
            NULL, AES_ENCRYPTION);
    }
    if (ret == 0) {
        /* reset messageTwo with the nvm read counter, pad with a 1 bit */
        *counter = (she_meta->count << 4);
        packet->sheLoadKeyReq.messageTwo[3] |= 0x08;
        /* encrypt the new counter */
        ret = wc_AesEncryptDirect(server->she->sheAes,
            packet->sheLoadKeyRes.messageFour + WOLFHSM_SHE_KEY_SZ,
            packet->sheLoadKeyReq.messageTwo);
    }
    /* free aes for protection */
    wc_AesFree(server->she->sheAes);
    /* generate K4 using the updated key */
    if (ret == 0) {
        /* set our UID, ID and AUTHID are already set from messageOne */
        XMEMCPY(packet->sheLoadKeyRes.messageFour, server->she->uid,
            sizeof(server->she->uid));
        /* add WOLFHSM_SHE_KEY_UPDATE_MAC_C to the input */
        XMEMCPY(kdfInput + meta->len, WOLFHSM_SHE_KEY_UPDATE_MAC_C,
            sizeof(WOLFHSM_SHE_KEY_UPDATE_MAC_C));
        /* do kdf */
        ret = wh_AesMp16(server, kdfInput,
            meta->len + sizeof(WOLFHSM_SHE_KEY_UPDATE_MAC_C), tmpKey);
    }
    /* cmac messageFour using K4 as the cmac key */
    if (ret == 0) {
        field = AES_BLOCK_SIZE;
        ret = wc_AesCmacGenerate_ex(server->she->sheCmac, packet->sheLoadKeyRes.messageFive,
            (word32*)&field, packet->sheLoadKeyRes.messageFour,
            sizeof(packet->sheLoadKeyRes.messageFour), tmpKey,
            WOLFHSM_SHE_KEY_SZ, NULL, server->crypto->devId);
    }
    if (ret == 0) {
        *size = WOLFHSM_PACKET_STUB_SIZE + sizeof(packet->sheLoadKeyRes);
        /* mark if the ram key was loaded */
        if ((meta->id & WOLFHSM_KEYID_MASK) == WOLFHSM_SHE_RAM_KEY_ID)
            server->she->ramKeyPlain = 1;
    }
    return ret;
}

static int hsmSheLoadPlainKey(whServerContext* server, whPacket* packet,
    uint16_t* size)
{
    int ret = 0;
    whNvmMetadata meta[1] = {{0}};
    meta->id = MAKE_WOLFHSM_KEYID(WOLFHSM_KEYTYPE_SHE,
        server->comm->client_id, WOLFHSM_SHE_RAM_KEY_ID);
    meta->len = WOLFHSM_SHE_KEY_SZ;
    /* cache if ram key, overwrite otherwise */
    ret = hsmCacheKey(server, meta, packet->sheLoadPlainKeyReq.key);
    if (ret == 0) {
        *size = WOLFHSM_PACKET_STUB_SIZE;
        server->she->ramKeyPlain = 1;
    }
    return ret;
}


static int hsmSheExportRamKey(whServerContext* server, whPacket* packet,
    uint16_t* size)
{
    int ret = 0;
    uint32_t keySz;
    uint32_t field;
    uint8_t kdfInput[WOLFHSM_SHE_KEY_SZ * 2];
    uint8_t cmacOutput[AES_BLOCK_SIZE];
    uint8_t tmpKey[WOLFHSM_SHE_KEY_SZ];
    whNvmMetadata meta[1] = {{0}};
    uint32_t* counter;

    /* check if ram key was loaded by CMD_LOAD_PLAIN_KEY */
    if (server->she->ramKeyPlain == 0)
        ret = WH_SHE_ERC_KEY_INVALID;
    /* read the auth key by AuthID */
    if (ret == 0) {
        keySz = WOLFHSM_SHE_KEY_SZ;
        ret = hsmReadKey(server, MAKE_WOLFHSM_KEYID(WOLFHSM_KEYTYPE_SHE,
            server->comm->client_id, WOLFHSM_SHE_SECRET_KEY_ID), meta,
            kdfInput, &keySz);
        if (ret != 0)
            ret = WH_SHE_ERC_KEY_NOT_AVAILABLE;
    }
    if (ret == 0) {
        /* set UID, key id and authId */
        XMEMCPY(packet->sheExportRamKeyRes.messageOne, server->she->uid,
            sizeof(server->she->uid));
        packet->sheExportRamKeyRes.messageOne[15] =
            ((WOLFHSM_SHE_RAM_KEY_ID << 4) | (WOLFHSM_SHE_SECRET_KEY_ID));
        /* add WOLFHSM_SHE_KEY_UPDATE_ENC_C to the input */
        XMEMCPY(kdfInput + meta->len, WOLFHSM_SHE_KEY_UPDATE_ENC_C,
            sizeof(WOLFHSM_SHE_KEY_UPDATE_ENC_C));
        /* generate K1 */
        ret = wh_AesMp16(server, kdfInput,
            meta->len + sizeof(WOLFHSM_SHE_KEY_UPDATE_ENC_C), tmpKey);
    }
    /* build cleartext M2 */
    if (ret == 0) {
        /* set the counter, flags and ram key */
        XMEMSET(packet->sheExportRamKeyRes.messageTwo, 0,
            sizeof(packet->sheExportRamKeyRes.messageTwo));
        /* set count to 1 */
        counter = (uint32_t*)packet->sheExportRamKeyRes.messageTwo;
        *counter = (wh_Utils_htonl(1) << 4);
        keySz = WOLFHSM_SHE_KEY_SZ;
        ret = hsmReadKey(server, MAKE_WOLFHSM_KEYID(WOLFHSM_KEYTYPE_SHE,
            server->comm->client_id, WOLFHSM_SHE_RAM_KEY_ID), meta,
            packet->sheExportRamKeyRes.messageTwo + WOLFHSM_SHE_KEY_SZ,
            &keySz);
        if (ret != 0)
            ret = WH_SHE_ERC_KEY_NOT_AVAILABLE;
    }
    /* encrypt M2 with K1 */
    if (ret == 0)
        ret = wc_AesInit(server->she->sheAes, NULL, server->crypto->devId);
    if (ret == 0) {
        ret = wc_AesSetKey(server->she->sheAes, tmpKey, WOLFHSM_SHE_KEY_SZ, NULL,
            AES_ENCRYPTION);
    }
    if (ret == 0) {
        /* copy the ram key to cmacOutput before it gets encrypted */
        XMEMCPY(cmacOutput,
            packet->sheExportRamKeyRes.messageTwo + WOLFHSM_SHE_KEY_SZ,
            WOLFHSM_SHE_KEY_SZ);
        ret = wc_AesCbcEncrypt(server->she->sheAes,
            packet->sheExportRamKeyRes.messageTwo,
            packet->sheExportRamKeyRes.messageTwo,
            sizeof(packet->sheExportRamKeyRes.messageTwo));
    }
    /* free aes for protection */
    wc_AesFree(server->she->sheAes);
    if (ret == 0) {
        /* add WOLFHSM_SHE_KEY_UPDATE_MAC_C to the input */
        XMEMCPY(kdfInput + meta->len, WOLFHSM_SHE_KEY_UPDATE_MAC_C,
            sizeof(WOLFHSM_SHE_KEY_UPDATE_MAC_C));
        /* generate K2 */
        ret = wh_AesMp16(server, kdfInput,
            meta->len + sizeof(WOLFHSM_SHE_KEY_UPDATE_MAC_C), tmpKey);
    }
    /* cmac messageOne and messageTwo using K2 as the cmac key */
    if (ret == 0) {
        field = AES_BLOCK_SIZE;
        ret = wc_AesCmacGenerate_ex(server->she->sheCmac,
            packet->sheExportRamKeyRes.messageThree, (word32*)&field,
            (uint8_t*)&packet->sheExportRamKeyRes,
            sizeof(packet->sheExportRamKeyRes.messageOne) +
            sizeof(packet->sheExportRamKeyRes.messageTwo), tmpKey,
            WOLFHSM_SHE_KEY_SZ, NULL, server->crypto->devId);
    }
    if (ret == 0) {
        /* copy the ram key to kdfInput */
        XMEMCPY(kdfInput, cmacOutput, WOLFHSM_SHE_KEY_SZ);
        /* add WOLFHSM_SHE_KEY_UPDATE_ENC_C to the input */
        XMEMCPY(kdfInput + WOLFHSM_SHE_KEY_SZ, WOLFHSM_SHE_KEY_UPDATE_ENC_C,
            sizeof(WOLFHSM_SHE_KEY_UPDATE_ENC_C));
        /* generate K3 */
        ret = wh_AesMp16(server, kdfInput,
            WOLFHSM_SHE_KEY_SZ + sizeof(WOLFHSM_SHE_KEY_UPDATE_ENC_C), tmpKey);
    }
    /* set K3 as encryption key */
    if (ret == 0)
        ret = wc_AesInit(server->she->sheAes, NULL, server->crypto->devId);
    if (ret == 0) {
        ret = wc_AesSetKey(server->she->sheAes, tmpKey, WOLFHSM_SHE_KEY_SZ,
            NULL, AES_ENCRYPTION);
    }
    if (ret == 0) {
        XMEMSET(packet->sheExportRamKeyRes.messageFour, 0,
            sizeof(packet->sheExportRamKeyRes.messageFour));
        /* set counter to 1, pad with 1 bit */
        counter = (uint32_t*)(packet->sheExportRamKeyRes.messageFour +
                WOLFHSM_SHE_KEY_SZ);
        *counter = (wh_Utils_htonl(1) << 4);
        packet->sheExportRamKeyRes.messageFour[WOLFHSM_SHE_KEY_SZ + 3] |=
            0x08;
        /* encrypt the new counter */
        ret = wc_AesEncryptDirect(server->she->sheAes,
            packet->sheExportRamKeyRes.messageFour + WOLFHSM_SHE_KEY_SZ,
            packet->sheExportRamKeyRes.messageFour + WOLFHSM_SHE_KEY_SZ);
    }
    /* free aes for protection */
    wc_AesFree(server->she->sheAes);
    if (ret == 0) {
        /* set UID, key id and authId */
        XMEMCPY(packet->sheExportRamKeyRes.messageFour, server->she->uid,
            sizeof(server->she->uid));
        packet->sheExportRamKeyRes.messageFour[15] =
            ((WOLFHSM_SHE_RAM_KEY_ID << 4) | (WOLFHSM_SHE_SECRET_KEY_ID));
        /* add WOLFHSM_SHE_KEY_UPDATE_MAC_C to the input */
        XMEMCPY(kdfInput + WOLFHSM_SHE_KEY_SZ, WOLFHSM_SHE_KEY_UPDATE_MAC_C,
            sizeof(WOLFHSM_SHE_KEY_UPDATE_MAC_C));
        /* generate K4 */
        ret = wh_AesMp16(server, kdfInput,
            WOLFHSM_SHE_KEY_SZ + sizeof(WOLFHSM_SHE_KEY_UPDATE_MAC_C), tmpKey);
    }
    /* cmac messageFour using K4 as the cmac key */
    if (ret == 0) {
        field = AES_BLOCK_SIZE;
        ret = wc_AesCmacGenerate_ex(server->she->sheCmac,
            packet->sheExportRamKeyRes.messageFive, (word32*)&field,
            packet->sheExportRamKeyRes.messageFour,
            sizeof(packet->sheExportRamKeyRes.messageFour), tmpKey,
            WOLFHSM_SHE_KEY_SZ, NULL, server->crypto->devId);
    }
    if (ret == 0)
        *size = WOLFHSM_PACKET_STUB_SIZE + sizeof(packet->sheExportRamKeyRes);
    return ret;
}

static int hsmSheInitRnd(whServerContext* server, whPacket* packet,
    uint16_t* size)
{
    int ret = 0;
    uint32_t keySz;
    uint8_t kdfInput[WOLFHSM_SHE_KEY_SZ * 2];
    uint8_t cmacOutput[AES_BLOCK_SIZE];
    uint8_t tmpKey[WOLFHSM_SHE_KEY_SZ];
    whNvmMetadata meta[1] = {{0}};
    /* check that init hasn't already been called since startup */
    if (server->she->rndInited == 1)
        ret = WH_SHE_ERC_SEQUENCE_ERROR;
    /* read secret key */
    if (ret == 0) {
        keySz = WOLFHSM_SHE_KEY_SZ;
        ret = hsmReadKey(server, MAKE_WOLFHSM_KEYID(WOLFHSM_KEYTYPE_SHE,
            server->comm->client_id, WOLFHSM_SHE_SECRET_KEY_ID), meta,
            kdfInput, &keySz);
        if (ret != 0)
            ret = WH_SHE_ERC_KEY_NOT_AVAILABLE;
    }
    if (ret == 0) {
        /* add PRNG_SEED_KEY_C */
        XMEMCPY(kdfInput + WOLFHSM_SHE_KEY_SZ, WOLFHSM_SHE_PRNG_SEED_KEY_C,
            sizeof(WOLFHSM_SHE_PRNG_SEED_KEY_C));
        /* generate PRNG_SEED_KEY */
        ret = wh_AesMp16(server, kdfInput,
            WOLFHSM_SHE_KEY_SZ + sizeof(WOLFHSM_SHE_PRNG_SEED_KEY_C), tmpKey);
    }
    /* read the current PRNG_SEED, i - 1, to cmacOutput */
    if (ret == 0) {
        keySz = WOLFHSM_SHE_KEY_SZ;
        ret = hsmReadKey(server, MAKE_WOLFHSM_KEYID(WOLFHSM_KEYTYPE_SHE,
            server->comm->client_id, WOLFHSM_SHE_PRNG_SEED_ID), meta,
            cmacOutput, &keySz);
        if (ret != 0)
            ret = WH_SHE_ERC_KEY_NOT_AVAILABLE;
    }
    /* set up aes */
    if (ret == 0)
        ret = wc_AesInit(server->she->sheAes, NULL, server->crypto->devId);
    if (ret == 0) {
        ret = wc_AesSetKey(server->she->sheAes, tmpKey, WOLFHSM_SHE_KEY_SZ,
            NULL, AES_ENCRYPTION);
    }
    /* encrypt to the PRNG_SEED, i */
    if (ret == 0) {
        ret = wc_AesCbcEncrypt(server->she->sheAes, cmacOutput, cmacOutput,
            WOLFHSM_SHE_KEY_SZ);
    }
    /* free aes for protection */
    wc_AesFree(server->she->sheAes);
    /* save PRNG_SEED, i */
    if (ret == 0) {
        meta->id = MAKE_WOLFHSM_KEYID(WOLFHSM_KEYTYPE_SHE,
            server->comm->client_id, WOLFHSM_SHE_PRNG_SEED_ID);
        meta->len = WOLFHSM_SHE_KEY_SZ;
        ret = wh_Nvm_AddObject(server->nvm, meta, meta->len, cmacOutput);
        if (ret != 0)
            ret = WH_SHE_ERC_KEY_UPDATE_ERROR;
    }
    if (ret == 0) {
        /* set PRNG_STATE */
        XMEMCPY(server->she->prngState, cmacOutput, WOLFHSM_SHE_KEY_SZ);
        /* add PRNG_KEY_C to the kdf input */
        XMEMCPY(kdfInput + WOLFHSM_SHE_KEY_SZ, WOLFHSM_SHE_PRNG_KEY_C,
            sizeof(WOLFHSM_SHE_PRNG_KEY_C));
        /* generate PRNG_KEY */
        ret = wh_AesMp16(server, kdfInput,
            WOLFHSM_SHE_KEY_SZ + sizeof(WOLFHSM_SHE_PRNG_KEY_C),
            server->she->prngKey);
    }
    if (ret == 0) {
        /* set init rng to 1 */
        server->she->rndInited = 1;
        /* set ERC_NO_ERROR */
        packet->sheInitRngRes.status = WOLFHSM_SHE_ERC_NO_ERROR;
        *size = WOLFHSM_PACKET_STUB_SIZE + sizeof(packet->sheInitRngRes);
    }
    return ret;
}


static int hsmSheRnd(whServerContext* server, whPacket* packet, uint16_t* size)
{
    int ret = 0;
    /* check that rng has been inited */
    if (server->she->rndInited == 0)
        ret = WH_SHE_ERC_RNG_SEED;
    /* set up aes */
    if (ret == 0)
        ret = wc_AesInit(server->she->sheAes, NULL, server->crypto->devId);
    /* use PRNG_KEY as the encryption key */
    if (ret == 0) {
        ret = wc_AesSetKey(server->she->sheAes, server->she->prngKey,
            WOLFHSM_SHE_KEY_SZ, NULL, AES_ENCRYPTION);
    }
    /* encrypt the PRNG_STATE, i - 1 to i */
    if (ret == 0) {
        ret = wc_AesCbcEncrypt(server->she->sheAes, server->she->prngState,
            server->she->prngState, WOLFHSM_SHE_KEY_SZ);
    }
    /* free aes for protection */
    wc_AesFree(server->she->sheAes);
    if (ret == 0) {
        /* copy PRNG_STATE */
        XMEMCPY(packet->sheRndRes.rnd, server->she->prngState,
            WOLFHSM_SHE_KEY_SZ);
        *size = WOLFHSM_PACKET_STUB_SIZE + sizeof(packet->sheRndRes);
    }
    return ret;
}

static int hsmSheExtendSeed(whServerContext* server, whPacket* packet,
    uint16_t* size)
{
    int ret = 0;
    uint32_t keySz;
    uint8_t kdfInput[WOLFHSM_SHE_KEY_SZ * 2];
    whNvmMetadata meta[1] = {{0}};
    /* check that rng has been inited */
    if (server->she->rndInited == 0)
        ret = WH_SHE_ERC_RNG_SEED;
    if (ret == 0) {
        /* set kdfInput to PRNG_STATE */
        XMEMCPY(kdfInput, server->she->prngState, WOLFHSM_SHE_KEY_SZ);
        /* add the user supplied entropy to kdfInput */
        XMEMCPY(kdfInput + WOLFHSM_SHE_KEY_SZ,
            packet->sheExtendSeedReq.entropy,
            sizeof(packet->sheExtendSeedReq.entropy));
        /* extend PRNG_STATE */
        ret = wh_AesMp16(server, kdfInput,
            WOLFHSM_SHE_KEY_SZ + sizeof(packet->sheExtendSeedReq.entropy),
            server->she->prngState);
    }
    /* read the PRNG_SEED into kdfInput */
    if (ret == 0) {
        keySz = WOLFHSM_SHE_KEY_SZ;
        ret = hsmReadKey(server, MAKE_WOLFHSM_KEYID(WOLFHSM_KEYTYPE_SHE,
            server->comm->client_id, WOLFHSM_SHE_PRNG_SEED_ID), meta,
            kdfInput, &keySz);
        if (ret != 0)
            ret = WH_SHE_ERC_KEY_NOT_AVAILABLE;
    }
    if (ret == 0) {
        /* extend PRNG_STATE */
        ret = wh_AesMp16(server, kdfInput,
            WOLFHSM_SHE_KEY_SZ + sizeof(packet->sheExtendSeedReq.entropy),
            kdfInput);
    }
    /* save PRNG_SEED */
    if (ret == 0) {
        meta->id = MAKE_WOLFHSM_KEYID(WOLFHSM_KEYTYPE_SHE,
            server->comm->client_id, WOLFHSM_SHE_PRNG_SEED_ID);
        meta->len = WOLFHSM_SHE_KEY_SZ;
        ret = wh_Nvm_AddObject(server->nvm, meta, meta->len, kdfInput);
        if (ret != 0)
            ret = WH_SHE_ERC_KEY_UPDATE_ERROR;
    }
    if (ret == 0) {
        /* set ERC_NO_ERROR */
        packet->sheExtendSeedRes.status = WOLFHSM_SHE_ERC_NO_ERROR;
        *size = WOLFHSM_PACKET_STUB_SIZE + sizeof(packet->sheExtendSeedRes);
    }
    return ret;
}

static int hsmSheEncEcb(whServerContext* server, whPacket* packet,
    uint16_t* size)
{
    int ret;
    uint32_t field;
    uint32_t keySz;
    uint8_t* in;
    uint8_t* out;
    uint8_t tmpKey[WOLFHSM_SHE_KEY_SZ];
    /* in and out are after the fixed sized fields */
    in = (uint8_t*)(&packet->sheEncEcbReq + 1);
    out = (uint8_t*)(&packet->sheEncEcbRes + 1);
    /* load the key */
    keySz = WOLFHSM_SHE_KEY_SZ;
    field = packet->sheEncEcbReq.sz;
    /* only process a multiple of block size */
    field -= (field % AES_BLOCK_SIZE);
    ret = hsmReadKey(server, MAKE_WOLFHSM_KEYID(WOLFHSM_KEYTYPE_SHE,
        server->comm->client_id, packet->sheEncEcbReq.keyId), NULL,
        tmpKey, &keySz);
    if (ret == 0)
        ret = wc_AesInit(server->she->sheAes, NULL, server->crypto->devId);
    else
        ret = WH_SHE_ERC_KEY_NOT_AVAILABLE;
    if (ret == 0)
        ret = wc_AesSetKey(server->she->sheAes, tmpKey, keySz, NULL, AES_ENCRYPTION);
    if (ret == 0)
        ret = wc_AesEcbEncrypt(server->she->sheAes, out, in, field);
    /* free aes for protection */
    wc_AesFree(server->she->sheAes);
    if (ret == 0) {
        packet->sheEncEcbRes.sz = field;
        *size = WOLFHSM_PACKET_STUB_SIZE + sizeof(packet->sheEncEcbRes) + field;
    }
    return ret;
}

static int hsmSheEncCbc(whServerContext* server, whPacket* packet,
    uint16_t* size)
{
    int ret;
    uint32_t field;
    uint32_t keySz;
    uint8_t* in;
    uint8_t* out;
    uint8_t tmpKey[WOLFHSM_SHE_KEY_SZ];
    /* in and out are after the fixed sized fields */
    in = (uint8_t*)(&packet->sheEncCbcReq + 1);
    out = (uint8_t*)(&packet->sheEncCbcRes + 1);
    /* load the key */
    keySz = WOLFHSM_SHE_KEY_SZ;
    field = packet->sheEncCbcReq.sz;
    /* only process a multiple of block size */
    field -= (field % AES_BLOCK_SIZE);
    ret = hsmReadKey(server, MAKE_WOLFHSM_KEYID(WOLFHSM_KEYTYPE_SHE,
        server->comm->client_id, packet->sheEncCbcReq.keyId), NULL,
        tmpKey, &keySz);
    if (ret == 0)
        ret = wc_AesInit(server->she->sheAes, NULL, server->crypto->devId);
    else
        ret = WH_SHE_ERC_KEY_NOT_AVAILABLE;
    if (ret == 0) {
        ret = wc_AesSetKey(server->she->sheAes, tmpKey, keySz, packet->sheEncCbcReq.iv,
            AES_ENCRYPTION);
    }
    if (ret == 0)
        ret = wc_AesCbcEncrypt(server->she->sheAes, out, in, field);
    /* free aes for protection */
    wc_AesFree(server->she->sheAes);
    if (ret == 0) {
        packet->sheEncEcbRes.sz = field;
        *size = WOLFHSM_PACKET_STUB_SIZE + sizeof(packet->sheEncCbcRes) + field;
    }
    return ret;
}

static int hsmSheDecEcb(whServerContext* server, whPacket* packet,
    uint16_t* size)
{
    int ret;
    uint32_t field;
    uint32_t keySz;
    uint8_t* in;
    uint8_t* out;
    uint8_t tmpKey[WOLFHSM_SHE_KEY_SZ];
    /* in and out are after the fixed sized fields */
    in = (uint8_t*)(&packet->sheDecEcbReq + 1);
    out = (uint8_t*)(&packet->sheDecEcbRes + 1);
    /* load the key */
    keySz = WOLFHSM_SHE_KEY_SZ;
    field = packet->sheDecEcbReq.sz;
    /* only process a multiple of block size */
    field -= (field % AES_BLOCK_SIZE);
    ret = hsmReadKey(server, MAKE_WOLFHSM_KEYID(WOLFHSM_KEYTYPE_SHE,
        server->comm->client_id, packet->sheDecEcbReq.keyId), NULL,
        tmpKey, &keySz);
    if (ret == 0)
        ret = wc_AesInit(server->she->sheAes, NULL, server->crypto->devId);
    else
        ret = WH_SHE_ERC_KEY_NOT_AVAILABLE;
    if (ret == 0)
        ret = wc_AesSetKey(server->she->sheAes, tmpKey, keySz, NULL, AES_DECRYPTION);
    if (ret == 0)
        ret = wc_AesEcbDecrypt(server->she->sheAes, out, in, field);
    /* free aes for protection */
    wc_AesFree(server->she->sheAes);
    if (ret == 0) {
        packet->sheDecEcbRes.sz = field;
        *size = WOLFHSM_PACKET_STUB_SIZE + sizeof(packet->sheDecEcbRes) + field;
    }
    return ret;
}

static int hsmSheDecCbc(whServerContext* server, whPacket* packet,
    uint16_t* size)
{
    int ret;
    uint32_t field;
    uint32_t keySz;
    uint8_t* in;
    uint8_t* out;
    uint8_t tmpKey[WOLFHSM_SHE_KEY_SZ];
    /* in and out are after the fixed sized fields */
    in = (uint8_t*)(&packet->sheDecCbcReq + 1);
    out = (uint8_t*)(&packet->sheDecCbcRes + 1);
    /* load the key */
    keySz = WOLFHSM_SHE_KEY_SZ;
    field = packet->sheDecCbcReq.sz;
    /* only process a multiple of block size */
    field -= (field % AES_BLOCK_SIZE);
    ret = hsmReadKey(server, MAKE_WOLFHSM_KEYID(WOLFHSM_KEYTYPE_SHE,
        server->comm->client_id, packet->sheDecCbcReq.keyId), NULL,
        tmpKey, &keySz);
    if (ret == 0)
        ret = wc_AesInit(server->she->sheAes, NULL, server->crypto->devId);
    else
        ret = WH_SHE_ERC_KEY_NOT_AVAILABLE;
    if (ret == 0) {
        ret = wc_AesSetKey(server->she->sheAes, tmpKey, keySz, packet->sheDecCbcReq.iv,
            AES_DECRYPTION);
    }
    if (ret == 0)
        ret = wc_AesCbcDecrypt(server->she->sheAes, out, in, field);
    /* free aes for protection */
    wc_AesFree(server->she->sheAes);
    if (ret == 0) {
        packet->sheDecCbcRes.sz = field;
        *size = WOLFHSM_PACKET_STUB_SIZE + sizeof(packet->sheDecCbcRes) + field;
    }
    return ret;
}

static int hsmSheGenerateMac(whServerContext* server, whPacket* packet,
    uint16_t* size)
{
    int ret;
    uint32_t field = AES_BLOCK_SIZE;
    uint32_t keySz;
    uint8_t* in;
    uint8_t tmpKey[WOLFHSM_SHE_KEY_SZ];
    /* in and out are after the fixed sized fields */
    in = (uint8_t*)(&packet->sheGenMacReq + 1);
    /* load the key */
    keySz = WOLFHSM_SHE_KEY_SZ;
    ret = hsmReadKey(server, MAKE_WOLFHSM_KEYID(WOLFHSM_KEYTYPE_SHE,
        server->comm->client_id, packet->sheGenMacReq.keyId), NULL, tmpKey,
        &keySz);
    /* hash the message */
    if (ret == 0) {
        ret = wc_AesCmacGenerate_ex(server->she->sheCmac, packet->sheGenMacRes.mac, (word32*)&field,
            in, packet->sheGenMacReq.sz, tmpKey, WOLFHSM_SHE_KEY_SZ, NULL,
            server->crypto->devId);
    }
    else
        ret = WH_SHE_ERC_KEY_NOT_AVAILABLE;
    if (ret == 0)
        *size = WOLFHSM_PACKET_STUB_SIZE + sizeof(packet->sheGenMacRes);
    return ret;
}

static int hsmSheVerifyMac(whServerContext* server, whPacket* packet,
    uint16_t* size)
{
    int ret;
    uint32_t keySz;
    uint8_t* message;
    uint8_t* mac;
    uint8_t tmpKey[WOLFHSM_SHE_KEY_SZ];
    /* in and mac are after the fixed sized fields */
    message = (uint8_t*)(&packet->sheVerifyMacReq + 1);
    mac = message + packet->sheVerifyMacReq.messageLen;
    /* load the key */
    keySz = WOLFHSM_SHE_KEY_SZ;
    ret = hsmReadKey(server, MAKE_WOLFHSM_KEYID(WOLFHSM_KEYTYPE_SHE,
        server->comm->client_id, packet->sheVerifyMacReq.keyId), NULL, tmpKey,
        &keySz);
    /* verify the mac */
    if (ret == 0) {
        ret = wc_AesCmacVerify_ex(server->she->sheCmac, mac, packet->sheVerifyMacReq.macLen,
            message, packet->sheVerifyMacReq.messageLen, tmpKey, keySz, NULL,
            server->crypto->devId);
        /* only evaluate if key was found */
        if (ret == 0)
            packet->sheVerifyMacRes.status = 0;
        else
            packet->sheVerifyMacRes.status = 1;
        *size = WOLFHSM_PACKET_STUB_SIZE + sizeof(packet->sheVerifyMacRes);
    }
    else
        ret = WH_SHE_ERC_KEY_NOT_AVAILABLE;
    return ret;
}

int wh_Server_HandleSheRequest(whServerContext* server,
    uint16_t action, uint8_t* data, uint16_t* size)
{
    int ret = 0;
    whPacket* packet = (whPacket*)data;

    if (server == NULL || data == NULL || size == NULL)
        return WH_ERROR_BADARGS;

    /* TODO does SHE specify what this error should be? */
    /* if we haven't secure booted, only allow secure boot requests */
    if ((server->she->sbState != WOLFHSM_SHE_SB_SUCCESS &&
        (action != WH_SHE_SECURE_BOOT_INIT &&
        action != WH_SHE_SECURE_BOOT_UPDATE &&
        action != WH_SHE_SECURE_BOOT_FINISH &&
        action != WH_SHE_GET_STATUS &&
        action != WH_SHE_SET_UID)) ||
        (action != WH_SHE_SET_UID && server->she->uidSet == 0)) {
        packet->rc = WH_SHE_ERC_SEQUENCE_ERROR;
        *size = WOLFHSM_PACKET_STUB_SIZE + sizeof(packet->rc);
        return 0;
    }

    switch (action)
    {
    case WH_SHE_SET_UID:
        ret = hsmSheSetUid(server, packet);
        break;
    case WH_SHE_SECURE_BOOT_INIT:
        ret = hsmSheSecureBootInit(server, packet, size);
        break;
    case WH_SHE_SECURE_BOOT_UPDATE:
        ret = hsmSheSecureBootUpdate(server, packet, size);
        break;
    case WH_SHE_SECURE_BOOT_FINISH:
        ret = hsmSheSecureBootFinish(server, packet, size);
        break;
    case WH_SHE_GET_STATUS:
        ret = hsmSheGetStatus(server, packet, size);
        break;
    case WH_SHE_LOAD_KEY:
        ret = hsmSheLoadKey(server, packet, size);
        break;
    case WH_SHE_LOAD_PLAIN_KEY:
        ret = hsmSheLoadPlainKey(server, packet, size);
        break;
    case WH_SHE_EXPORT_RAM_KEY:
        ret = hsmSheExportRamKey(server, packet, size);
        break;
    case WH_SHE_INIT_RND:
        ret = hsmSheInitRnd(server, packet, size);
        break;
    case WH_SHE_RND:
        ret = hsmSheRnd(server, packet, size);
        break;
    case WH_SHE_EXTEND_SEED:
        ret = hsmSheExtendSeed(server, packet, size);
        break;
    case WH_SHE_ENC_ECB:
        ret = hsmSheEncEcb(server, packet, size);
        break;
    case WH_SHE_ENC_CBC:
        ret = hsmSheEncCbc(server, packet, size);
        break;
    case WH_SHE_DEC_ECB:
        ret = hsmSheDecEcb(server, packet, size);
        break;
    case WH_SHE_DEC_CBC:
        ret = hsmSheDecCbc(server, packet, size);
        break;
    case WH_SHE_GEN_MAC:
        ret = hsmSheGenerateMac(server, packet, size);
        break;
    case WH_SHE_VERIFY_MAC:
        ret = hsmSheVerifyMac(server, packet, size);
        break;
    default:
        ret = WH_ERROR_BADARGS;
        break;
    }
    /* if a handler didn't set a specific error, set general error */
    if (ret != 0) {
        if (ret != WH_SHE_ERC_SEQUENCE_ERROR &&
        ret != WH_SHE_ERC_KEY_NOT_AVAILABLE && ret != WH_SHE_ERC_KEY_INVALID &&
        ret != WH_SHE_ERC_KEY_EMPTY && ret != WH_SHE_ERC_NO_SECURE_BOOT &&
        ret != WH_SHE_ERC_WRITE_PROTECTED && ret != WH_SHE_ERC_KEY_UPDATE_ERROR
        && ret != WH_SHE_ERC_RNG_SEED && ret != WH_SHE_ERC_NO_DEBUGGING &&
        ret != WH_SHE_ERC_BUSY && ret != WH_SHE_ERC_MEMORY_FAILURE) {
            ret = WH_SHE_ERC_GENERAL_ERROR;
        }
        packet->rc = ret;
        *size = WOLFHSM_PACKET_STUB_SIZE + sizeof(packet->rc);
    }
    /* reset our SHE state */
    /* TODO is it safe to call wc_InitCmac over and over or do we need to call final first? */
    if ((action == WH_SHE_SECURE_BOOT_INIT ||
        action == WH_SHE_SECURE_BOOT_UPDATE ||
        action == WH_SHE_SECURE_BOOT_FINISH) && ret != 0 &&
        ret != WH_SHE_ERC_NO_SECURE_BOOT) {
        server->she->sbState = WOLFHSM_SHE_SB_INIT;
        server->she->blSize = 0;
        server->she->blSizeReceived = 0;
        server->she->cmacKeyFound = 0;
    }
    packet->rc = ret;
    return 0;
}

#else /* WOLFHSM_NO_CRYPTO */
int wh_Server_HandleSheRequest(whServerContext* server,
    uint16_t action, uint8_t* data, uint16_t* size)
{
    /* No crypto build, so always return bad args */
    (void)server;
    (void)action;
    (void)data;
    (void)size;
    return WH_ERROR_BADARGS;
}

#endif /* !WOLFHSM_NO_CRYPTO */

#endif /* WOLFHSM_SHE_EXTENSION*/
