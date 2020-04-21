/*******************************************************************************
* Fantom Ledger App
* (c) 2020 Fantom Foundation
*
* Some parts of the code are derived from Ledger Ethereum App distributed under
* Apache License, Version 2.0. You may obtain a copy of the License at
*
*      http://www.apache.org/licenses/LICENSE-2.0
*
********************************************************************************/
#include <stdint.h>

#include "common.h"
#include "utils.h"
#include "rlp_utils.h"
#include "tx_stream.h"
#include "transaction.h"
#include "errors.h"

// txStreamInit implements new transaction stream initialization.
void txStreamInit(
        tx_stream_context_t *ctx,
        cx_sha3_t *sha3,
        transaction_t *tx
) {
    // clear the context
    os_memset(ctx, 0, sizeof(tx_stream_context_t));

    // assign SHA3 and init the SHA3 context
    ctx->sha3 = sha3;
    cx_keccak_init(ctx->sha3, 256);

    // keep the transaction content reference
    ctx->tx = tx;

    // assign initial expected value
    // TX_RLP_ENVELOPE is the transaction envelope list since
    // the transaction is sent as RLP encoded list of values
    ctx->currentField = TX_RLP_ENVELOPE;
}

// txStreamReadByte implements reading singe byte of data from the stream work buffer.
// We use it to detect length field in the incoming data which precedes all the data
// fields except self-encoded single byte data elements.
static uint8_t txStreamReadByte(tx_stream_context_t *ctx) {
    uint8_t data;

    // make sure we are safely withing a current command length
    if (ctx->workBufferLength < 1) {
        THROW(ERR_INVALID_DATA);
    }

    // read the data from work buffer and advance pointers
    data = *ctx->workBuffer;
    ctx->workBuffer++;
    ctx->workBufferLength--;

    // advance field position so we track position in field parsing
    if (ctx->isProcessingField) {
        ctx->currentFieldPos++;
    }

    // add the field hash to SHA3 context if appropriate
    // we hash everything what isn't single byte field element
    if (!(ctx->isProcessingField && ctx->isFieldSingleByte)) {
        cx_hash((cx_hash_t *) ctx->sha3, 0, &data, 1, NULL, 0);
    }

    return data;
}

// txStreamCopyData implements copying data from transaction stream into an output buffer.
static void txStreamCopyData(tx_stream_context_t *ctx, uint8_t *out, uint32_t length) {
    // validate we are safely inside the current command length
    if (ctx->workBufferLength < length) {
        THROW(ERR_INVALID_DATA);
    }

    // make sure the output buffer is valid before we move the data
    if (out != NULL) {
        os_memmove(out, ctx->workBuffer, length);
    }

    // add the data hash to SHA3 context if appropriate
    // we hash everything what isn't single byte field element
    if (!(ctx->isProcessingField && ctx->isFieldSingleByte)) {
        cx_hash((cx_hash_t *) ctx->sha3, 0, ctx->workBuffer, length, NULL, 0);
    }

    // advance the work buffer and clear the command length we already processed
    ctx->workBuffer += length;
    ctx->workBufferLength -= length;

    // if processing field, mark the advancement on that field as well
    if (ctx->isProcessingField) {
        ctx->currentFieldPos += length;
    }
}

// txStreamProcessContent handles tx content processing.
// The content represents the top level envelope for list of actual tx values.
static void txStreamProcessContent(tx_stream_context_t *ctx) {
    // the content should be marked as a list of values
    VALIDATE(ctx->isCurrentFieldList, ERR_INVALID_DATA);

    // keep data length reference for sanity checks
    ctx->dataLength = ctx->currentFieldLength;

    // advance expected field processing to the next one
    // see tx_stream.h for list of expected tx fields
    ctx->currentField++;
    ctx->isProcessingField = false;
}

// txStreamProcessInt256Field implements transaction field processing.
// If the field is NULL the data is actually not stored and are thrown away.
static void txStreamProcessInt256Field(tx_stream_context_t *ctx, tx_int256_t *field, uint32_t fieldSize) {
    // the field must not be marked as a list of values, it's a single value
    VALIDATE(!ctx->isCurrentFieldList, ERR_INVALID_DATA);

    // make sure the length is appropriate; it has to fit in the provided buffer
    VALIDATE(ctx->currentFieldLength <= fieldSize, ERR_INVALID_DATA);

    // are we safely inside the field length?
    if (ctx->currentFieldPos < ctx->currentFieldLength) {
        // determine the length of skip we need to make
        uint32_t toCopy = (ctx->workBufferLength < (ctx->currentFieldLength - ctx->currentFieldPos) ?
                           ctx->workBufferLength : ctx->currentFieldLength - ctx->currentFieldPos);

        // copy into the target field, or throw away the data and just move to the next element?
        if (field != NULL) {
            // copy data to target field on the correct position
            txStreamCopyData(ctx, field->value + ctx->currentFieldPos, toCopy);
        } else {
            // just throw the data
            txStreamCopyData(ctx, NULL, toCopy);
        }
    }

    // are we done with the field?
    // if so, move processing to the next field
    if (ctx->currentFieldPos == ctx->currentFieldLength) {
        // set the field real size if any
        if (field != NULL) {
            field->length = ctx->currentFieldLength;
        }

        // advance the field
        ctx->currentField++;
        ctx->isProcessingField = false;
    }
}

// txStreamProcessAddressField implements transaction field processing.
// If the field is NULL the data is actually not stored and are thrown away.
static void txStreamProcessAddressField(tx_stream_context_t *ctx, tx_address_t *address, uint32_t fieldSize) {
    // the field must not be marked as a list of values, it's a single value
    VALIDATE(!ctx->isCurrentFieldList, ERR_INVALID_DATA);

    // make sure the length is appropriate; it has to fit in the provided buffer
    VALIDATE(ctx->currentFieldLength <= fieldSize, ERR_INVALID_DATA);

    // are we safely inside the field length?
    if (ctx->currentFieldPos < ctx->currentFieldLength) {
        // determine the length of skip we need to make
        uint32_t toCopy = (ctx->workBufferLength < (ctx->currentFieldLength - ctx->currentFieldPos) ?
                           ctx->workBufferLength : ctx->currentFieldLength - ctx->currentFieldPos);

        // copy into the target field, or throw away the data and just move to the next element?
        if (address != NULL) {
            // copy data to target field on the correct position
            txStreamCopyData(ctx, address->value + ctx->currentFieldPos, toCopy);
        } else {
            // just throw the data
            txStreamCopyData(ctx, NULL, toCopy);
        }
    }

    // are we done with the field?
    // if so, move processing to the next field
    if (ctx->currentFieldPos == ctx->currentFieldLength) {
        // set the field real size if any
        if (address != NULL) {
            address->length = ctx->currentFieldLength;
        }

        // advance the field
        ctx->currentField++;
        ctx->isProcessingField = false;
    }
}

// txStreamProcessVField implements transaction V field processing.
// If the field is NULL the data is actually not stored and are thrown away.
static void txStreamProcessVField(tx_stream_context_t *ctx, tx_v_t *v, uint32_t fieldSize) {
    // the field must not be marked as a list of values, it's a single value
    VALIDATE(!ctx->isCurrentFieldList, ERR_INVALID_DATA);

    // make sure the length is appropriate; it has to fit in the provided buffer
    VALIDATE(ctx->currentFieldLength <= fieldSize, ERR_INVALID_DATA);

    // are we safely inside the field length?
    if (ctx->currentFieldPos < ctx->currentFieldLength) {
        // determine the length of skip we need to make
        uint32_t toCopy = (ctx->workBufferLength < (ctx->currentFieldLength - ctx->currentFieldPos) ?
                           ctx->workBufferLength : ctx->currentFieldLength - ctx->currentFieldPos);

        // copy into the target field, or throw away the data and just move to the next element?
        if (v != NULL) {
            // copy data to target field on the correct position
            txStreamCopyData(ctx, v->value + ctx->currentFieldPos, toCopy);
        } else {
            // just throw the data
            txStreamCopyData(ctx, NULL, toCopy);
        }
    }

    // are we done with the field?
    // if so, move processing to the next field
    if (ctx->currentFieldPos == ctx->currentFieldLength) {
        // set the field real size if any
        if (v != NULL) {
            v->length = ctx->currentFieldLength;
        }

        // advance the field
        ctx->currentField++;
        ctx->isProcessingField = false;
    }
}


// txStreamParseFieldProxy implements current field parsing proxy.
static tx_stream_status_e txStreamParseFieldProxy(tx_stream_context_t *ctx) {
    // decide what to do based on the current field parsed
    switch (ctx->currentField) {
        case TX_RLP_ENVELOPE:
            // parse the transaction envelope
            txStreamProcessContent(ctx);

            // do we have a type in the TX transfer?
            // if not, skip the TX_RLP_TYPE field
            if ((ctx->processingFlags & TX_FLAG_TYPE) == 0) {
                ctx->currentField++;
            }
            break;
        case TX_RLP_TYPE:
            // we don't keep the type, but we do run parse on it since
            // all the incoming data participate on SHA3 hash building
            txStreamProcessInt256Field(ctx, NULL, TX_MAX_INT256_LENGTH);
            break;
        case TX_RLP_NONCE:
            // we don't keep the sender's address nonce
            txStreamProcessInt256Field(ctx, NULL, TX_MAX_INT256_LENGTH);
            break;
        case TX_RLP_GAS_PRICE:
            txStreamProcessInt256Field(ctx, &ctx->tx->gasPrice, TX_MAX_INT256_LENGTH);
            break;
        case TX_RLP_START_GAS:
            txStreamProcessInt256Field(ctx, &ctx->tx->startGas, TX_MAX_INT256_LENGTH);
            break;
        case TX_RLP_VALUE:
            txStreamProcessInt256Field(ctx, &ctx->tx->value, TX_MAX_INT256_LENGTH);
            break;
        case TX_RLP_RECIPIENT:
            txStreamProcessAddressField(ctx, &ctx->tx->recipient, TX_MAX_ADDRESS_LENGTH);
            break;
        case TX_RLP_DATA:
        case TX_RLP_R:
        case TX_RLP_S:
            // those fields can be arbitrary and we don't track their size
            // we also don't need the content so we don't store them anywhere
            txStreamProcessInt256Field(ctx, NULL, MAX_BUFFER_SIZE);
            break;
        case TX_RLP_V:
            // this could/should contain chain identification
            txStreamProcessVField(ctx, &ctx->tx->v, TX_MAX_INT256_LENGTH);
            break;
        default:
            // we don't allow unknown fields to be processed
            return TX_STREAM_FAULT;
    }

    return TX_STREAM_PROCESSING;
}

// txStreamDetectField tries to detect field length block in the current data buffer.
static tx_stream_status_e txStreamDetectField(tx_stream_context_t *ctx, bool *canDecode) {
    // we assume the decoding can not be done by default
    *canDecode = false;

    // read to the end of current buffer
    while (ctx->workBufferLength != 0) {
        bool isValid;

        // validate if we are safely within the RLP buffer size
        if (ctx->rlpBufferPos == RLP_LENGTH_BUFFER_SIZE) {
            return TX_STREAM_FAULT;
        }

        // feed the RLP buffer until the new field length can be decoded
        // or until we run out of data in the current wire buffer
        ctx->rlpBuffer[ctx->rlpBufferPos++] = txStreamReadByte(ctx);

        // check if the current RLP buffer can reveal field
        if (rlpCanDecode(ctx->rlpBuffer, ctx->rlpBufferPos, &isValid)) {
            // can not decode invalid data stream
            if (!isValid) {
                return TX_STREAM_FAULT;
            }

            // we can decode here
            *canDecode = true;
            break;
        }
    }

    return TX_STREAM_PROCESSING;
}

// txStreamParse implements incoming buffer parser.
static tx_stream_status_e txStreamParse(tx_stream_context_t *ctx) {
    // loop until the buffer is parsed
    for (;;) {
        // are we done with the parsing?
        if (ctx->currentField == TX_RLP_DONE) {
            return TX_STREAM_FINISHED;
        }

        // old school transactions don't have the "v" value and anything beyond ("r" and "s")
        if ((ctx->currentField == TX_RLP_V) && (ctx->workBufferLength == 0)) {
            ctx->tx->v.length = 0;
            return TX_STREAM_FINISHED;
        }

        // did we reach the end of this wire buffer?
        if (ctx->workBufferLength == 0) {
            return TX_STREAM_PROCESSING;
        }

        // we are at an edge of a new field and we need to try to detect
        // the new field length and prepare the context to parse this
        // field from the RLP buffer
        if (!ctx->isProcessingField) {
            bool canDecode;
            uint32_t offset;

            // try to detect next field in the current command buffer
            if (txStreamDetectField(ctx, &canDecode) == TX_STREAM_FAULT) {
                return TX_STREAM_FAULT;
            }

            // if the current buffer can not decode next field, just continue APDU com
            if (!canDecode) {
                return TX_STREAM_PROCESSING;
            }

            // decode field length and type
            if (!rlpDecodeLength(ctx->rlpBuffer, ctx->rlpBufferPos, &ctx->currentFieldLength, &offset,
                                 &ctx->isCurrentFieldList)) {
                return TX_STREAM_FAULT;
            }

            // reset RLP buffer, we used it to get the length
            ctx->rlpBufferPos = 0;

            // is this a single byte self encoded field?
            // in RLP encoding the first byte between 0x00 and 0x7f represent the actual value
            // and no offset is needed to decode the value since the next byte is already a new field
            ctx->isFieldSingleByte = (offset == 0);

            // hack for self encoded single byte
            if (offset == 0) {
                ctx->workBuffer--;
                ctx->workBufferLength++;
            }

            // now we are processing a field
            ctx->currentFieldPos = 0;
            ctx->isProcessingField = true;
        }//(!ctx->isProcessingField)

        // parse the current field
        if (txStreamParseFieldProxy(ctx) == TX_STREAM_FAULT) {
            return TX_STREAM_FAULT;
        }
    }
}

// txStreamProcess implements processing of a buffer of data into the transaction stream.
tx_stream_status_e txStreamProcess(
        tx_stream_context_t *ctx,
        uint8_t *buffer,
        uint32_t length,
        uint32_t flags
) {
    // collect parser result
    tx_stream_status_e result;

    // try to pick where we ended and continue parsing
    BEGIN_TRY
    {
        TRY
        {
            // validate we have at least some data to process
            VALIDATE(length > 0, ERR_INVALID_DATA);

            // make sure we are actually waiting for a field
            // the initial state is beyond TX_RLP_NONE and the TX_RLP_DONE means we don't need anything else
            VALIDATE(ctx->currentField > TX_RLP_NONE && ctx->currentField < TX_RLP_DONE, ERR_INVALID_DATA);

            // assign the buffer to context
            ctx->workBuffer = buffer;
            ctx->workBufferLength = length;
            ctx->processingFlags = flags;

            // run stream handler
            result = txStreamParse(ctx);
        }
        CATCH_OTHER(e)
        {
            result = TX_STREAM_FAULT;
        }
        FINALLY
        {
        }
    }
    END_TRY;

    return result;
}