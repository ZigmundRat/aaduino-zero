/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2017 Johan Kanflo (github.com/kanflo)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "dbg_printf.h"
#include "tick.h"
#include "rfm69.h"
#include "rfm69_link.h"


/** Dump incoming packets */
//#define CONFIG_LINK_RX_DEBUG


#define LINK_OVERHEAD       (3) // <dst> <src> <counter & flags>

#define LINK_TX_RETRY_COUNT       (5) // Number of TX attempts
#define LINK_TX_RETRY_SLEEP_MS  (500) // Sleep time between TX attempts

static uint8_t _counter = 0;
static uint8_t _node_id = 0;


/**
 * @brief      Set node ID for this link node
 *
 * @param[in]  node_id  Node id
 */
void rfm69link_setNodeId(uint8_t node_id)
{
    _node_id = node_id;
    rfm69_setAddress(node_id);
}

/**
 * @brief      Set network ID for this link node
 *
 * @param[in]  net_id  Network id
 */
void rfm69link_setNetworkId(uint8_t net_id)
{
    rfm69_setNetwork(net_id);
}

/**
 * @brief      Send frame over RFM69 link
 *
 * @param[in]  dst      Destinaton address
 * @param      payload  Payload (max RFM69_LINK_MAX_FRAME bytes)
 * @param[in]  length   Payload length
 *
 * @return     rfm69_ack for successful transmission
 *             rfm69_ack | rfm69_lat if receiver requested LAT
 *             0 for failure 
 */
uint8_t rfm69link_sendFrame(uint8_t dst, rfm69_link_frame_t *frame, uint8_t length)
{
    uint8_t tx_len, tx_status = 0;
    uint8_t attempt = 0;
    if (!frame) {
        return 0;
    }
    frame->_dst = dst;
    frame->_src = _node_id;
    frame->_cntr_flags = ((_counter & FRAME_FLAGS_MASK) << 4) | rfm69_req_ack;
    do {
#ifdef CONFIG_LINK_RX_DEBUG
        dbg_printf("TX attempt #%d\n", attempt);
#endif // CONFIG_LINK_RX_DEBUG
        tx_len = rfm69_send((uint8_t*) frame, LINK_OVERHEAD + length);
        if (tx_len > 0) {
            rfm69_link_frame_t ack;
            uint8_t len = rfm69_receive((char*) &ack, LINK_OVERHEAD + RFM69_LINK_MAX_FRAME);
            if (!len) {
                /** @todo: listen to RFM pin for RX done, or at lest query the darn thing */
                delay_ms(100);
                len = rfm69_receive((char*) &ack, LINK_OVERHEAD + RFM69_LINK_MAX_FRAME);
            }
#ifdef CONFIG_LINK_RX_DEBUG
            dbg_printf("Got ack\n");
#endif // CONFIG_LINK_RX_DEBUG
            tx_len = len;
            rfm69_sleep();
            if (!len) {
#ifdef CONFIG_LINK_RX_DEBUG
                dbg_printf("No ACK\n");
#endif // CONFIG_LINK_RX_DEBUG
                delay_ms(LINK_TX_RETRY_SLEEP_MS); // @todo: low power sleep
            } else {
                if (FRAME_COUNTER(frame->_cntr_flags) != FRAME_COUNTER(ack._cntr_flags)) {
                    dbg_printf("Error: sender acked frame %d when I sent %d\n", FRAME_COUNTER(ack._cntr_flags), FRAME_COUNTER(frame->_cntr_flags));
                    /** @todo: handle */
                }
                tx_status = FRAME_FLAGS(ack._cntr_flags);
                _counter = (_counter + 1) & FRAME_FLAGS_MASK;
                frame->rssi = rfm69_getRSSI();
            }
        }
        attempt++;
    } while(attempt < LINK_TX_RETRY_COUNT && tx_len == 0);

    rfm69_sleep();
    return tx_status;
}

/**
 * @brief      Receive a frame
 *
 * @param      src      Address of transmitter
 * @param      frame    Frame for payload
 * @param      length   Length of received payload
 *
 * @return     true if frame was received
 */
bool rfm69link_receiveFrame(uint8_t *src, rfm69_link_frame_t *frame, uint8_t *length)
{
    if (!src || !frame || !length) {
        dbg_printf("rfm69link_receiveFrame: param error\n");
        return false;
    }
    *length = 0;
    bool success = false;
    uint8_t len = rfm69_receive((char*) frame, LINK_OVERHEAD + RFM69_LINK_MAX_FRAME);
    if (len >= LINK_OVERHEAD) {
        frame->rssi = rfm69_getRSSI();
        rfm69_sleep();
#ifdef CONFIG_LINK_RX_DEBUG
        uint8_t *p = (uint8_t*) frame;
        dbg_printf("Link: %d byte frame (%02d -> %02d) [ ", len, frame->_src, frame->_dst);
        for (int i = 0; i < len; i++) {
            dbg_printf("%02x ", p[i]);
        }
        dbg_printf("] rssi:%d\n", frame->rssi);
#endif // CONFIG_LINK_RX_DEBUG
        if (frame->_dst == _node_id) {
            *length = len - LINK_OVERHEAD;
            *src = frame->_src;
            success = true;
            if (FRAME_FLAGS(frame->_cntr_flags) & rfm69_req_ack) {
                rfm69_link_frame_t ack; /** @todo: optimize stack usage */
                ack._src = frame->_dst;
                ack._dst = frame->_src;
                /** sender's counter + ack bit */
                ack._cntr_flags = (frame->_cntr_flags & (FRAME_COUNTER_MASK << FRAME_COUNTER_SHIFT)) | rfm69_ack;
#ifdef CONFIG_LINK_RX_DEBUG
                dbg_printf("Sending ack\n");
#endif // CONFIG_LINK_RX_DEBUG
                len = rfm69_send((uint8_t*) &ack, LINK_OVERHEAD);
#ifdef CONFIG_LINK_RX_DEBUG
                dbg_printf("TX ack:%d\n", len);
#endif // CONFIG_LINK_RX_DEBUG
            }
        }
    }
    return success;
}
