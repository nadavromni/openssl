/*
 * Copyright 2022 The OpenSSL Project Authors. All Rights Reserved.
 *
 * Licensed under the Apache License 2.0 (the "License").  You may not use
 * this file except in compliance with the License.  You can obtain a copy
 * in the file LICENSE in the source distribution or at
 * https://www.openssl.org/source/license.html
 */

#include "internal/quic_fifd.h"
#include "internal/quic_wire.h"

DEFINE_LIST_OF(tx_history, OSSL_ACKM_TX_PKT);

int ossl_quic_fifd_init(QUIC_FIFD *fifd,
                        QUIC_CFQ *cfq,
                        OSSL_ACKM *ackm,
                        QUIC_TXPIM *txpim,
                        /* stream_id is UINT64_MAX for the crypto stream */
                        QUIC_SSTREAM *(*get_sstream_by_id)(uint64_t stream_id,
                                                           void *arg),
                        void *get_sstream_by_id_arg,
                        /* stream_id is UINT64_MAX if not applicable */
                        void (*regen_frame)(uint64_t frame_type,
                                            uint64_t stream_id,
                                            void *arg),
                        void *regen_frame_arg)
{
    if (cfq == NULL || ackm == NULL || txpim == NULL
        || get_sstream_by_id == NULL || regen_frame == NULL)
        return 0;

    fifd->cfq                   = cfq;
    fifd->ackm                  = ackm;
    fifd->txpim                 = txpim;
    fifd->get_sstream_by_id     = get_sstream_by_id;
    fifd->get_sstream_by_id_arg = get_sstream_by_id_arg;
    fifd->regen_frame           = regen_frame;
    fifd->regen_frame_arg       = regen_frame_arg;
    return 1;
}

void ossl_quic_fifd_cleanup(QUIC_FIFD *fifd)
{
    /* No-op. */
}

static void on_acked(void *arg)
{
    QUIC_TXPIM_PKT *pkt = arg;
    QUIC_FIFD *fifd = pkt->fifd;
    const QUIC_TXPIM_CHUNK *chunks = ossl_quic_txpim_pkt_get_chunks(pkt);
    size_t i, num_chunks = ossl_quic_txpim_pkt_get_num_chunks(pkt);
    QUIC_SSTREAM *sstream;
    QUIC_CFQ_ITEM *cfq_item, *cfq_item_next;

    /* STREAM and CRYPTO stream chunks, FINs and stream FC frames */
    for (i = 0; i < num_chunks; ++i) {
        sstream = fifd->get_sstream_by_id(chunks[i].stream_id,
                                          fifd->get_sstream_by_id_arg);
        if (sstream == NULL)
            continue;

        if (chunks[i].end >= chunks[i].start)
            ossl_quic_sstream_mark_acked(sstream,
                                         chunks[i].start, chunks[i].end);

        if (chunks[i].has_fin && chunks[i].stream_id != UINT64_MAX)
            ossl_quic_sstream_mark_acked_fin(sstream);
    }

    /* GCR */
    for (cfq_item = pkt->retx_head; cfq_item != NULL; cfq_item = cfq_item_next) {
        cfq_item_next = cfq_item->pkt_next;
        ossl_quic_cfq_release(fifd->cfq, cfq_item);
    }

    ossl_quic_txpim_pkt_release(fifd->txpim, pkt);
}

static void on_lost(void *arg)
{
    QUIC_TXPIM_PKT *pkt = arg;
    QUIC_FIFD *fifd = pkt->fifd;
    const QUIC_TXPIM_CHUNK *chunks = ossl_quic_txpim_pkt_get_chunks(pkt);
    size_t i, num_chunks = ossl_quic_txpim_pkt_get_num_chunks(pkt);
    QUIC_SSTREAM *sstream;
    QUIC_CFQ_ITEM *cfq_item, *cfq_item_next;

    /* STREAM and CRYPTO stream chunks, FIN and stream FC frames */
    for (i = 0; i < num_chunks; ++i) {
        sstream = fifd->get_sstream_by_id(chunks[i].stream_id,
                                          fifd->get_sstream_by_id_arg);
        if (sstream == NULL)
            continue;

        if (chunks[i].end >= chunks[i].start)
            ossl_quic_sstream_mark_lost(sstream,
                                        chunks[i].start, chunks[i].end);

        if (chunks[i].has_fin && chunks[i].stream_id != UINT64_MAX)
            ossl_quic_sstream_mark_lost_fin(sstream);

        /*
         * Inform caller that stream needs an FC frame.
         *
         * Note: We could track whether an FC frame was sent originally for the
         * stream to determine if it really needs to be regenerated or not.
         * However, if loss has occurred, it's probably better to ensure the
         * peer has up-to-date flow control data for the stream. Given that
         * these frames are extremely small, we may as well always send it when
         * handling loss.
         */
        fifd->regen_frame(OSSL_QUIC_FRAME_TYPE_MAX_STREAM_DATA,
                          chunks[i].stream_id,
                          fifd->regen_frame_arg);
    }

    /* GCR */
    for (cfq_item = pkt->retx_head; cfq_item != NULL; cfq_item = cfq_item_next) {
        cfq_item_next = cfq_item->pkt_next;
        ossl_quic_cfq_mark_lost(fifd->cfq, cfq_item, UINT32_MAX);
    }

    /* Regenerate flag frames */
    if (pkt->had_handshake_done_frame)
        fifd->regen_frame(OSSL_QUIC_FRAME_TYPE_HANDSHAKE_DONE,
                          UINT64_MAX,
                          fifd->regen_frame_arg);

    if (pkt->had_max_data_frame)
        fifd->regen_frame(OSSL_QUIC_FRAME_TYPE_MAX_DATA,
                          UINT64_MAX,
                          fifd->regen_frame_arg);

    if (pkt->had_max_streams_bidi_frame)
        fifd->regen_frame(OSSL_QUIC_FRAME_TYPE_MAX_STREAMS_BIDI,
                          UINT64_MAX,
                          fifd->regen_frame_arg);

    if (pkt->had_max_streams_uni_frame)
        fifd->regen_frame(OSSL_QUIC_FRAME_TYPE_MAX_STREAMS_UNI,
                          UINT64_MAX,
                          fifd->regen_frame_arg);

    if (pkt->had_ack_frame)
        /*
         * We always use the ACK_WITH_ECN frame type to represent the ACK frame
         * type in our callback; we assume it is the caller's job to decide
         * whether it wants to send ECN data or not.
         */
        fifd->regen_frame(OSSL_QUIC_FRAME_TYPE_ACK_WITH_ECN,
                          UINT64_MAX,
                          fifd->regen_frame_arg);

    ossl_quic_txpim_pkt_release(fifd->txpim, pkt);
}

static void on_discarded(void *arg)
{
    QUIC_TXPIM_PKT *pkt = arg;
    QUIC_FIFD *fifd = pkt->fifd;
    QUIC_CFQ_ITEM *cfq_item, *cfq_item_next;

    /*
     * Don't need to do anything to SSTREAMs for STREAM and CRYPTO streams, as
     * we assume caller will clean them up.
     */

    /* GCR */
    for (cfq_item = pkt->retx_head; cfq_item != NULL; cfq_item = cfq_item_next) {
        cfq_item_next = cfq_item->pkt_next;
        ossl_quic_cfq_release(fifd->cfq, cfq_item);
    }

    ossl_quic_txpim_pkt_release(fifd->txpim, pkt);
}

int ossl_quic_fifd_pkt_commit(QUIC_FIFD *fifd, QUIC_TXPIM_PKT *pkt)
{
    QUIC_CFQ_ITEM *cfq_item;

    pkt->fifd                   = fifd;

    pkt->ackm_pkt.on_lost       = on_lost;
    pkt->ackm_pkt.on_acked      = on_acked;
    pkt->ackm_pkt.on_discarded  = on_discarded;
    pkt->ackm_pkt.cb_arg        = pkt;

    ossl_list_tx_history_init_elem(&pkt->ackm_pkt);
    pkt->ackm_pkt.anext = pkt->ackm_pkt.lnext = NULL;

    /*
     * Mark the CFQ items which have been added to this packet as having been
     * transmitted.
     */
    for (cfq_item = pkt->retx_head;
         cfq_item != NULL;
         cfq_item = cfq_item->pkt_next)
        ossl_quic_cfq_mark_tx(fifd->cfq, cfq_item);

    /* Inform the ACKM. */
    return ossl_ackm_on_tx_packet(fifd->ackm, &pkt->ackm_pkt);
}
