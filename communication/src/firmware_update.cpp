/*
 * Copyright (c) 2020 Particle Industries, Inc.  All rights reserved.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation, either
 * version 3 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "firmware_update.h"

#if HAL_PLATFORM_OTA_PROTOCOL_V3

#include "message_channel.h"
#include "coap_message_encoder.h"
#include "coap_message_decoder.h"

#include "sha256.h"
#include "endian_util.h"
#include "logging.h"
#include "check.h"

#include <algorithm>

LOG_SOURCE_CATEGORY("comm.ota")

namespace particle::protocol {

namespace {

static_assert(PARTICLE_LITTLE_ENDIAN, "This code is optimized for little-endian architectures");

// Protocol-specific CoAP options
enum MessageOption {
    CHUNK_INDEX = 2049,
    WINDOW_SIZE = 2053,
    FILE_SIZE = 2057,
    FILE_SHA_256 = 2061,
    CHUNK_SIZE = 2065,
    DISCARD_DATA = 2069,
    CANCEL_UPDATE = 2073
};

inline unsigned trailingOneBits(uint32_t v) {
    v = ~v;
    if (!v) {
        return 32;
    }
    return __builtin_ctz(v);
}

} // namespace

FirmwareUpdate::FirmwareUpdate(MessageChannel* channel, FirmwareUpdateCallbacks callbacks) :
        ctx_(callbacks.userData()),
        callbacks_(std::move(callbacks)),
        channel_(channel),
        updating_(false) {
    reset();
}

FirmwareUpdate::~FirmwareUpdate() {
}

int FirmwareUpdate::process() {
    if (!updating_) {
        return 0;
    }
    if (unackChunks_ > 0 && lastAckTime_ + OTA_CHUNK_ACK_DELAY <= millis()) {
        // Send a ChunkAck message
        size_t ackPayloadSize = 0;
        for (int i = OTA_CHUNK_BITMAP_ELEMENTS - 1; i >= 0; --i) {
            if (chunks_[i]) {
                ackPayloadSize = (i + 1) * sizeof(uint32_t);
                break;
            }
        }
        Message msg;
        int r = channel_->create(msg);
        if (r != ProtocolError::NO_ERROR) {
            LOG(ERROR, "Failed to create message");
            return r;
        }
        CoapMessageEncoder e((char*)msg.buf(), msg.capacity());
        e.type(CoapType::NON);
        e.id(0); // Will be assigned by the message channel
        e.option(MessageOption::CHUNK_INDEX, chunkIndex_);
        e.payload((const char*)chunks_, ackPayloadSize);
        r = e.encode();
        if (r < 0) {
            LOG(ERROR, "Failed to encode message: %d", r);
            return ProtocolError::UNKNOWN;
        }
        msg.set_length(r);
        r = channel_->send(msg);
        if (r != ProtocolError::NO_ERROR) {
            LOG(ERROR, "Failed to send message: %d", (int)r);
            return ProtocolError::IO_ERROR_GENERIC_SEND;
        }
        lastAckTime_ = millis();
        unackChunks_ = 0;
    }
    return 0;
}

int FirmwareUpdate::handleRequest(Message* msg, RequestHandlerFn handler) {
    CoapMessageDecoder d;
    int r = d.decode((const char*)msg->buf(), msg->length());
    if (r < 0) {
        LOG(ERROR, "Failed to decode message: %d", r);
        return ProtocolError::MALFORMED_MESSAGE;
    }
    if (d.type() == CoapType::CON) {
        // Send an empty ACK. Make sure not to overwrite the contents of the original message
        const size_t avail = msg->capacity() - msg->length();
        Message ack;
        r = channel_->response(*msg, ack, avail);
        if (r != ProtocolError::NO_ERROR) {
            LOG(ERROR, "Failed to create message");
            return r;
        }
        r = sendEmptyAck(&ack, CoapType::ACK, d.id());
        if (r < 0) {
            LOG(ERROR, "Failed to send ACK: %d", r);
            return ProtocolError::IO_ERROR_GENERIC_SEND;
        }
    }
    // TODO: All messages created via BufferMessageChannel share the same buffer. It is typically
    // not safe to start encoding a response message until the original request is fully processed
    Message resp;
    r = channel_->create(resp);
    if (r != ProtocolError::NO_ERROR) {
        LOG(ERROR, "Failed to create message");
        return r;
    }
    CoapMessageEncoder e((char*)resp.buf(), resp.capacity());
    const int handlerResult = (this->*handler)(d, &e);
    if (handlerResult >= 0) {
        // Encode and send the response message
        r = e.encode();
        if (r >= 0) {
            resp.set_length(r);
            r = channel_->send(*msg);
            if (r != ProtocolError::NO_ERROR) {
                LOG(ERROR, "Failed to send message: %d", (int)r);
                return ProtocolError::IO_ERROR_GENERIC_SEND;
            }
        } else if (r != SYSTEM_ERROR_INVALID_STATE) {
            LOG(ERROR, "Failed to encode message: %d", r);
            return ProtocolError::UNKNOWN;
        } // SYSTEM_ERROR_INVALID_STATE indicates that the request handler hasn't encoded a response
    } else {
        if (d.tokenSize() > 0) {
            // Reply with a separate confirmable or non-confirmable response
            r = sendErrorResponse(&resp, handlerResult, d.type(), d.token(), d.tokenSize());
            if (r < 0) {
                LOG(ERROR, "Failed to send response: %d", r);
                return ProtocolError::IO_ERROR_GENERIC_SEND;
            }
        } else {
            // The original request doesn't have a token so we can only reply with an RST to it
            r = sendEmptyAck(&resp, CoapType::RST, d.id());
            if (r < 0) {
                LOG(ERROR, "Failed to send RST: %d", r);
                return ProtocolError::IO_ERROR_GENERIC_SEND;
            }
        }
        // Errors during an OTA update are not fatal unless we weren't able to send a response to
        // indicate the error
        reset();
    }
    return 0;
}

int FirmwareUpdate::handleBeginRequest(const CoapMessageDecoder& d, CoapMessageEncoder* e) {
    if (!updating_) {
        LOG(INFO, "Received Begin request");
    } else {
        LOG(WARN, "Received Begin request but another update is already in progress");
    }
    reset();
    updateStartTime_ = millis();
    // Parse message
    const char* fileHash = nullptr;
    bool discardData = false;
    CHECK(parseBeginRequest(d, &fileHash, &fileSize_, &chunkSize_, &discardData));
    // Start the update
    FirmwareUpdateFlags flags;
    if (discardData) {
        flags |= FirmwareUpdateFlag::DISCARD_DATA;
    }
    if (!fileHash) {
        flags |= FirmwareUpdateFlag::NON_RESUMABLE;
    }
    LOG(INFO, "Starting firmware update");
    LOG(INFO, "File size: %u", (unsigned)fileSize_);
    LOG(INFO, "Chunk size: %u", (unsigned)chunkSize_);
    LOG(INFO, "Discard data: %u", (unsigned)discardData);
    if (fileHash) {
        LOG(INFO, "File checksum:");
        LOG_DUMP(INFO, fileHash, Sha256::HASH_SIZE);
        LOG_PRINT(INFO, "\r\n");
    }
    CHECK(startUpdate(fileSize_, fileHash, &fileOffset_, flags));
    transferSize_ = fileSize_ - fileOffset_;
    chunkCount_ = (transferSize_ + chunkSize_ - 1) / chunkSize_;
    LOG(INFO, "File offset: %u", (unsigned)fileOffset_);
    LOG(INFO, "Chunk count: %u", (unsigned)chunkCount_);
    windowSize_ = OTA_RECEIVE_WINDOW_SIZE / chunkSize_;
#if OTA_UPDATE_STATS
    LOG(INFO, "Window size (chunks): %u", (unsigned)windowSize_);
#endif
    updating_ = true;
    e->type(d.type());
    e->id(0); // Will be assigned by the message channel
    e->token(d.token(), d.tokenSize());
    e->option(MessageOption::FILE_SIZE, fileOffset_);
    return 0;
}

int FirmwareUpdate::handleEndRequest(const CoapMessageDecoder& d, CoapMessageEncoder* e) {
    if (!updating_) {
        LOG(WARN, "Received End request but no update is in progress");
        return SYSTEM_ERROR_INVALID_STATE;
    }
    bool cancelUpdate = false;
    bool discardData = false;
    CHECK(parseEndRequest(d, &cancelUpdate, &discardData));
    LOG(INFO, "Finishing firmware update");
    LOG(INFO, "Cancel update: %u", (unsigned)cancelUpdate);
    LOG(INFO, "Discard data: %u", (unsigned)discardData);
    FirmwareUpdateFlags flags;
    if (cancelUpdate) {
        flags |= FirmwareUpdateFlag::CANCEL;
    } else if (fileOffset_ != fileSize_) {
        LOG(ERROR, "Incomplete file transfer");
        return SYSTEM_ERROR_PROTOCOL;
    }
    if (discardData) {
        if (!cancelUpdate) {
            LOG(ERROR, "Invalid request options");
            return SYSTEM_ERROR_PROTOCOL;
        }
        flags |= FirmwareUpdateFlag::DISCARD_DATA;
    }
    flags |= FirmwareUpdateFlag::VALIDATE_ONLY;
    CHECK(finishUpdate(flags));
    updating_ = false;
    e->type(d.type());
    e->id(0); // Will be assigned by the message channel
    e->token(d.token(), d.tokenSize());
    return 0;
}

int FirmwareUpdate::handleChunkRequest(const CoapMessageDecoder& d, CoapMessageEncoder* e) {
    if (!updating_) {
        LOG(WARN, "Received Chunk request but no update is in progress");
        return SYSTEM_ERROR_INVALID_STATE;
    }
    const char* data = nullptr;
    size_t size = 0;
    unsigned index = 0;
    CHECK(parseChunkRequest(d, &data, &size, &index));
    if (index == 0 || index > chunkCount_) { // Chunk indices are 1-based
        LOG(ERROR, "Invalid chunk index: %u", index);
        return SYSTEM_ERROR_PROTOCOL;
    }
    if ((index < chunkCount_ && size != chunkSize_) ||
            (index == chunkCount_ && (index - 1) * chunkSize_ + size > transferSize_)) {
        LOG(ERROR, "Invalid chunk size: %u", size);
        return SYSTEM_ERROR_PROTOCOL;
    }
    bool dupChunk = false;
    bool outWindowChunk = false;
    if (index <= chunkIndex_) {
        dupChunk = true;
        outWindowChunk = true;
    } else if (index > chunkIndex_ + windowSize_) {
        outWindowChunk = true;
    } else {
        // Index of the chunk relative to the left edge of the receiver window (0-based)
        const unsigned relIndex = index - chunkIndex_ - 1;
        // Position of the chunk bit in the bitmap
        const size_t wordIndex = relIndex / 32;
        const unsigned bitIndex = relIndex % 32;
        uint32_t w = chunks_[wordIndex];
        if (w & (1 << bitIndex)) {
            dupChunk = true;
        } else {
            w |= (1 << bitIndex);
            chunks_[wordIndex] = w;
            const size_t offs = fileOffset_ + relIndex * chunkSize_; // Chunk offset in the file
            if (relIndex == 0) {
                // Shift the receiver window
                unsigned bits = 0;
                while ((bits = trailingOneBits(chunks_[0]))) {
                    for (size_t i = 0; i < OTA_CHUNK_BITMAP_ELEMENTS; ++i) {
                        chunks_[i] >>= bits;
                        if (i != OTA_CHUNK_BITMAP_ELEMENTS - 1) {
                            chunks_[i] |= chunks_[i + 1] << (32 - bits);
                        }
                    }
                    fileOffset_ += bits * chunkSize_;
                    chunkIndex_ += bits;
                }
            }
            // Last chunk can be smaller than chunkSize_
            if (fileOffset_ > fileSize_) {
                fileOffset_ = fileSize_;
            }
            CHECK(saveChunk(data, size, offs, fileOffset_));
        }
    }
    bool hasGaps = false;
    size_t ackPayloadSize = 0;
    for (int i = OTA_CHUNK_BITMAP_ELEMENTS - 1; i >= 0; --i) {
        // If some bits are still set in the bitmap, then there's a gap in the sequence of received chunks
        if (chunks_[i]) {
            ackPayloadSize = (i + 1) * sizeof(uint32_t);
            hasGaps = true;
            break;
        }
    }
    ++unackChunks_;
    if (hasGaps || dupChunk || outWindowChunk || unackChunks_ == OTA_CHUNK_ACK_COUNT ||
            lastAckTime_ == 0 || lastAckTime_ + OTA_CHUNK_ACK_DELAY <= millis()) {
        // Send a ChunkAck message
        e->type(CoapType::NON);
        e->id(0); // Will be assigned by the message channel
        e->option(MessageOption::CHUNK_INDEX, chunkIndex_);
        e->payload((const char*)chunks_, ackPayloadSize);
        lastAckTime_ = millis();
        unackChunks_ = 0;
#if OTA_UPDATE_STATS
        if (dupChunk) {
            ++dupChunks_;
        }
        if (outWindowChunk) {
            ++outWindowChunks_;
        }
#endif // OTA_UPDATE_STATS
    }
    return 0;
}

int FirmwareUpdate::sendErrorResponse(Message* msg, int error, CoapType type, const char* token, size_t tokenSize) {
    CoapMessageEncoder e((char*)msg->buf(), msg->capacity());
    e.type(type);
    e.code(coapCodeForSystemError(error));
    e.id(0); // Will be assigned by the message channel
    e.token(token, tokenSize);
    int r = e.encode();
    if (r < 0) {
        LOG(ERROR, "Failed to encode message: %d", r);
        return r;
    }
    if (r > (int)msg->capacity()) {
        LOG(ERROR, "Too large message");
        return SYSTEM_ERROR_TOO_LARGE;
    }
    msg->set_length(r);
    r = channel_->send(*msg);
    if (r != ProtocolError::NO_ERROR) {
        LOG(ERROR, "Failed to send message: %d", (int)r);
        return SYSTEM_ERROR_IO;
    }
    return 0;
}

int FirmwareUpdate::sendEmptyAck(Message* msg, CoapType type, CoapMessageId id) {
    CoapMessageEncoder e((char*)msg->buf(), msg->capacity());
    e.type(type);
    e.code(CoapCode::EMPTY);
    e.id(id);
    int r = e.encode();
    if (r < 0) {
        LOG(ERROR, "Failed to encode message: %d", r);
        return r;
    }
    if (r > (int)msg->capacity()) {
        LOG(ERROR, "Too large message");
        return SYSTEM_ERROR_TOO_LARGE;
    }
    msg->set_length(r);
    r = channel_->send(*msg);
    if (r != ProtocolError::NO_ERROR) {
        LOG(ERROR, "Failed to send message: %d", (int)r);
        return SYSTEM_ERROR_IO;
    }
    return 0;
}

int FirmwareUpdate::parseBeginRequest(const CoapMessageDecoder& d, const char** fileHash, size_t* fileSize,
        size_t* chunkSize, bool* discardData) {
    if (d.type() != CoapType::CON) {
        LOG(ERROR, "Invalid message type");
        return SYSTEM_ERROR_PROTOCOL;
    }
    if (d.tokenSize() != sizeof(token_t)) {
        LOG(ERROR, "Invalid token size");
        return SYSTEM_ERROR_PROTOCOL;
    }
    bool hasFileSize = false;
    bool hasFileHash = false;
    bool hasChunkSize = false;
    bool hasDiscardData = false;
    auto it = d.options();
    while (it.next()) {
        switch (it.option()) {
        case MessageOption::FILE_SIZE: {
            const size_t size = it.toUInt();
            if (!size) {
                LOG(ERROR, "Invalid file size: %u", (unsigned)size);
                return SYSTEM_ERROR_PROTOCOL;
            }
            *fileSize = size;
            hasFileSize = true;
            break;
        }
        case MessageOption::FILE_SHA_256: {
            if (it.size() != Sha256::HASH_SIZE) {
                LOG(ERROR, "Invalid option size");
                return SYSTEM_ERROR_PROTOCOL;
            }
            *fileHash = it.data();
            hasFileHash = true;
            break;
        }
        case MessageOption::CHUNK_SIZE: {
            const size_t size = it.toUInt();
            if (!size || size < MIN_OTA_CHUNK_SIZE || size > MAX_OTA_CHUNK_SIZE) {
                LOG(ERROR, "Invalid chunk size: %u", (unsigned)size);
                return SYSTEM_ERROR_PROTOCOL;
            }
            *chunkSize = size;
            hasChunkSize = true;
            break;
        }
        case MessageOption::DISCARD_DATA: {
            if (it.size() != 0) {
                LOG(ERROR, "Invalid option size");
                return SYSTEM_ERROR_PROTOCOL;
            }
            *discardData = true;
            hasDiscardData = true;
            break;
        }
        default:
            break;
        }
    }
    if (!hasFileSize || !hasChunkSize) {
        LOG(ERROR, "Mandatory option is missing");
        return SYSTEM_ERROR_PROTOCOL;
    }
    if (!hasFileHash) {
        *fileHash = nullptr;
    }
    if (!hasDiscardData) {
        *discardData = false;
    }
    return 0;
}

int FirmwareUpdate::parseEndRequest(const CoapMessageDecoder& d, bool* cancelUpdate, bool* discardData) {
    if (d.type() != CoapType::CON) {
        LOG(ERROR, "Invalid message type");
        return SYSTEM_ERROR_PROTOCOL;
    }
    if (d.tokenSize() != sizeof(token_t)) {
        LOG(ERROR, "Invalid token size");
        return SYSTEM_ERROR_PROTOCOL;
    }
    bool hasDiscardData = false;
    bool hasCancelUpdate = false;
    auto it = d.options();
    while (it.next()) {
        switch (it.option()) {
        case MessageOption::DISCARD_DATA: {
            if (it.size() != 0) {
                LOG(ERROR, "Invalid option size");
                return SYSTEM_ERROR_PROTOCOL;
            }
            *discardData = true;
            hasDiscardData = true;
            break;
        }
        case MessageOption::CANCEL_UPDATE: {
            if (it.size() != 0) {
                LOG(ERROR, "Invalid option size");
                return SYSTEM_ERROR_PROTOCOL;
            }
            *cancelUpdate = true;
            hasCancelUpdate = true;
            break;
        }
        default:
            break;
        }
    }
    if (!hasDiscardData) {
        *discardData = false;
    }
    if (!hasCancelUpdate) {
        *cancelUpdate = false;
    }
    return 0;
}

int FirmwareUpdate::parseChunkRequest(const CoapMessageDecoder& d, const char** chunkData, size_t* chunkSize,
        unsigned* chunkIndex) {
    if (d.type() != CoapType::NON) {
        LOG(ERROR, "Invalid message type");
        return SYSTEM_ERROR_PROTOCOL;
    }
    if (d.tokenSize() != 0) {
        LOG(ERROR, "Invalid token size");
        return SYSTEM_ERROR_PROTOCOL;
    }
    if (d.payloadSize() == 0) {
        LOG(ERROR, "Invalid payload size");
        return SYSTEM_ERROR_PROTOCOL;
    }
    const auto it = d.findOption(MessageOption::CHUNK_INDEX);
    if (!it) {
        LOG(ERROR, "Mandatory option is missing");
        return SYSTEM_ERROR_PROTOCOL;
    }
    *chunkIndex = it.toUInt();
    *chunkData = d.payload();
    *chunkSize = d.payloadSize();
    return 0;
}

void FirmwareUpdate::reset() {
    if (updating_) {
        const int r = finishUpdate(FirmwareUpdateFlag::CANCEL);
        if (r < 0) {
            LOG(ERROR, "Failed to cancel the update: %d", r);
        }
        updating_ = false;
    }
    ctx_.reset();
    memset(chunks_, 0, sizeof(chunks_));
    updateStartTime_ = 0;
    lastChunkTime_ = 0;
    lastAckTime_ = 0;
    fileSize_ = 0;
    fileOffset_ = 0;
    transferSize_ = 0;
    chunkSize_ = 0;
    chunkCount_ = 0;
    windowSize_ = 0;
    chunkIndex_ = 0;
    unackChunks_ = 0;
#if OTA_UPDATE_STATS
    processTime_ = 0;
    recvChunks_ = 0;
    sentAcks_ = 0;
    outOrderChunks_ = 0;
    outWindowChunks_ = 0;
    dupChunks_ = 0;
#endif // OTA_UPDATE_STATS
}

} // namespace particle::protocol

#endif // HAL_PLATFORM_OTA_PROTOCOL_V3
