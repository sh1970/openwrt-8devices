/*
 * Copyright (c) 2017-2021 The Linux Foundation. All rights reserved.
 * Copyright (c) 2021-2022 Qualcomm Innovation Center, Inc. All rights reserved.
 *
 * Permission to use, copy, modify, and/or distribute this software for
 * any purpose with or without fee is hereby granted, provided that the
 * above copyright notice and this permission notice appear in all
 * copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL
 * WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE
 * AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL
 * DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR
 * PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

#include <htt.h>
#include "qdf_trace.h"
#include "qdf_nbuf.h"
#include "dp_peer.h"
#include "dp_types.h"
#include "dp_internal.h"
#include "htt_ppdu_stats.h"
#include "dp_htt.h"
#include "qdf_mem.h"   /* qdf_mem_malloc,free */
#include "cdp_txrx_cmn_struct.h"
#include <enet.h>

#include <linux/debugfs.h>
#include <linux/uaccess.h>
#include "qdf_lock.h"
#include "qdf_debugfs.h"
#include "dp_rx.h"
#include "dp_mon.h"
#include "dp_rx_mon.h"
#include "dp_tx_capture.h"

#include <dp_rx_mon_1.0.h>

#define NUM_BITS_DWORD 32

#define MAX_MONITOR_HEADER (512)
#define MAX_DUMMY_FRM_BODY (128)
#define DP_BA_ACK_FRAME_SIZE (sizeof(struct ieee80211_ctlframe_addr2) + 36)
#define DP_ACK_FRAME_SIZE (sizeof(struct ieee80211_frame_min_one))
#define DP_CTS_FRAME_SIZE (sizeof(struct ieee80211_frame_min_one))
#define DP_ACKNOACK_FRAME_SIZE (sizeof(struct ieee80211_frame) + 16)
#define DP_MAX_MPDU_64 64
#define DP_NUM_WORDS_PER_PPDU_BITMAP_64 (DP_MAX_MPDU_64 >> 5)
#define DP_NUM_BYTES_PER_PPDU_BITMAP_64 (DP_MAX_MPDU_64 >> 3)
#define DP_NUM_BYTES_PER_PPDU_BITMAP (HAL_RX_MAX_MPDU >> 3)
#define DP_IEEE80211_BAR_CTL_TID_S 12
#define DP_IEEE80211_BAR_CTL_TID_M 0xf
#define DP_IEEE80211_BAR_CTL_POLICY_S 0
#define DP_IEEE80211_BAR_CTL_POLICY_M 0x1
#define DP_IEEE80211_BA_S_SEQ_S 4
#define DP_IEEE80211_BAR_CTL_COMBA 0x0004

#define INVALID_PPDU_ID 0xFFFF
#define MAX_END_TSF 0xFFFFFFFF

#define DP_IEEE80211_CATEGORY_VHT (21)
#define DP_NOACK_SOUNDING_TOKEN_POS (4)
#define DP_NOACK_STOKEN_POS_SHIFT (2)
#define DP_NDPA_TOKEN_POS (16)

#define IEEE80211_FC1_SHIFT (8)

/* Macros to handle sequence number bitmaps */

/* HW generated rts frame flag */
#define SEND_WIFIRTS_LEGACY_E 1

/* HW generated 11 AC static bw flag */
#define SEND_WIFIRTS_11AC_STATIC_BW_E 2

/* HW generated 11 AC dynamic bw flag */
#define SEND_WIFIRTS_11AC_DYNAMIC_BW_E 3

/* HW generated cts frame flag */
#define SEND_WIFICTS2SELF_E 4

/* Size (in bits) of a segment of sequence number bitmap */
#define SEQ_SEG_SZ_BITS(_seqarr) (sizeof(_seqarr[0]) << 3)

/* Array index of a segment of sequence number bitmap */
#define SEQ_SEG_INDEX(_seqarr, _seqno) ((_seqno) / SEQ_SEG_SZ_BITS(_seqarr))

/* Bit mask of a seqno within a segment of sequence bitmap */
#define SEQ_SEG_MSK(_seqseg, _index) \
	(1 << ((_index) & ((sizeof(_seqseg) << 3) - 1)))

/* Check seqno bit in a segment of sequence bitmap */
#define SEQ_SEG_BIT(_seqseg, _index) \
	((_seqseg) & SEQ_SEG_MSK((_seqseg), _index))

/* Segment of sequence bitmap containing a given sequence number */
#define SEQ_SEG(_seqarr, _seqno) \
	(_seqarr[(_seqno) / (sizeof(_seqarr[0]) << 3)])

/* Check seqno bit in the sequence bitmap */
#define SEQ_BIT(_seqarr, _seqno) \
	SEQ_SEG_BIT(SEQ_SEG(_seqarr, (_seqno)), (_seqno))

/* Lower 32 mask for timestamp us as completion path has 32 bits timestamp */
#define LOWER_32_MASK 0xFFFFFFFF

/* Maximum time taken to enqueue next mgmt pkt */
#define MAX_MGMT_ENQ_DELAY 10000

/* Schedule id counter mask in ppdu_id */
#define SCH_ID_MASK 0xFF

#define IEEE80211_IS_ZERO(_a)				\
	((_a)[0] == 0x00 &&				\
	 (_a)[1] == 0x00 &&				\
	 (_a)[2] == 0x00 &&				\
	 (_a)[3] == 0x00 &&				\
	 (_a)[4] == 0x00 &&				\
	 (_a)[5] == 0x00)
/* Maximum number of retries */
#define MAX_RETRY_Q_COUNT 20

#define DP_PEER_TX_TID_INIT_DONE_BIT 0

#ifdef WLAN_TX_PKT_CAPTURE_ENH

#define CHECK_MPDUS_NULL(ptr_val)					\
	{								\
		if (qdf_unlikely(ptr_val)) {				\
			dp_tx_capture_alert(				\
					    "already stored value over written");	\
			QDF_BUG(0);					\
		}							\
	}

#define TX_CAP_WDI_EVENT_HANDLER(_soc_, _pdev_id_, payload)		\
	{								\
		dp_wdi_event_handler(WDI_EVENT_TX_PKT_CAPTURE,		\
				     _soc_,				\
				     payload, HTT_INVALID_PEER,		\
				     WDI_NO_VAL,			\
				     _pdev_id_);			\
	}

static inline bool dp_tx_capt_mem_check(struct dp_pdev *pdev, int buf_size)
{
	struct dp_mon_soc *mon_soc = pdev->soc->monitor_soc;

	if ((qdf_atomic_read(&mon_soc->dp_soc_tx_capt.ppdu_bytes) +
		 qdf_atomic_read(&mon_soc->dp_soc_tx_capt.ppdu_mgmt_bytes) +
		 buf_size) >=
	    wlan_cfg_get_tx_capt_max_mem(pdev->soc->wlan_cfg_ctx)) {
		return false;
	} else {
		return true;
	}
}

static uint32_t get_queue_bytes(qdf_nbuf_queue_t *q)
{
	qdf_nbuf_t nbuf;
	uint32_t num_bytes = 0;

	nbuf = qdf_nbuf_queue_first(q);
	while (nbuf) {
		num_bytes += qdf_nbuf_get_truesize(nbuf);
		nbuf = qdf_nbuf_queue_next(nbuf);
	}
	return num_bytes;
}

#ifdef WLAN_TX_PKT_CAPTURE_ENH_DEBUG

#define TX_CAP_NBUF_QUEUE_FREE(q_head)					\
	tx_cap_nbuf_queue_free_test(q_head, __func__, __LINE__)

/**
 * check_queue_empty() - check queue is empty if not assert
 * @qhead: head of queue
 *
 * Return: void
 */
static inline void check_queue_empty(qdf_nbuf_queue_t *qhead)
{
	if (qdf_unlikely(!qdf_nbuf_is_queue_empty(qhead))) {
		dp_tx_capture_alert("Queue is not empty len(%d) !!",
				    qdf_nbuf_queue_len(qhead));
		QDF_BUG(0);
	}
}

/**
 * tx_cap_nbuf_queue_free_test() - free a queue and check queue length matching
 * element to be free.
 * @qhead: head of queue
 * @func: caller name
 * @line: line number
 *
 * Return: QDF status
 */
static inline QDF_STATUS
tx_cap_nbuf_queue_free_test(qdf_nbuf_queue_t *qhead,
			    const char *func, uint32_t line)
{
	qdf_nbuf_t  buf = NULL;
	uint32_t len = 0;
	uint32_t actual_len = 0;

	if (qhead) {
		len = qhead->qlen;
		actual_len = qhead->qlen;
	}

	while ((buf = qdf_nbuf_queue_remove(qhead)) != NULL) {
		qdf_nbuf_free(buf);
		len--;
	}

	if (len) {
		dp_tx_capture_alert("Error actual len: [u], mem_free len: [%u]\n",
				    actual_len, len);
		QDF_BUG(0);
	}

	return QDF_STATUS_SUCCESS;
}

/* stats counter */
/**
 * dp_tx_cap_stats_msdu_update() - update msdu level stats counter per peer
 * @peer: DP PEER object
 * @msdu_desc: msdu desc index
 * @count: count to update
 *
 * Return: void
 */
static inline
void dp_tx_cap_stats_msdu_update(struct dp_peer *peer,
				 uint8_t msdu_desc, uint32_t count)
{
	struct dp_peer_tx_capture_stats *stats;

	stats = &peer->monitor_peer->tx_capture.stats;

	stats->msdu[msdu_desc] += count;
}

/**
 * dp_tx_cap_stats_mpdu_update() - update mpdu level stats counter per peer
 * @peer: DP PEER object
 * @mpdu_desc: mpdu desc index
 * @count: count to update
 *
 * Return: void
 */
static inline
void dp_tx_cap_stats_mpdu_update(struct dp_peer *peer,
				 uint8_t mpdu_desc, uint32_t count)
{
	struct dp_peer_tx_capture_stats *stats;

	stats = &peer->monitor_peer->tx_capture.stats;

	stats->mpdu[mpdu_desc] += count;
}

/**
 * dp_tx_capture_print_stats() - print stats counter per peer
 * @peer: DP PEER object
 *
 * Return: void
 */
static inline
void dp_tx_capture_print_stats(struct dp_peer *peer)
{
	struct dp_peer_tx_capture_stats *stats;

	stats = &peer->monitor_peer->tx_capture.stats;
	DP_PRINT_STATS(" peer_id[%d] MSDU[S:%u E:%u D:%u F:%u DP:%u X:%u] MPDU[T:%u S:%u R:%u A:%u C:%u ST:%u]",
		       peer->peer_id,
		       stats->msdu[PEER_MSDU_SUCC],
		       stats->msdu[PEER_MSDU_ENQ],
		       stats->msdu[PEER_MSDU_DEQ],
		       stats->msdu[PEER_MSDU_FLUSH],
		       stats->msdu[PEER_MSDU_DROP],
		       stats->msdu[PEER_MSDU_XRETRY],
		       stats->mpdu[PEER_MPDU_TRI],
		       stats->mpdu[PEER_MPDU_SUCC],
		       stats->mpdu[PEER_MPDU_RESTITCH],
		       stats->mpdu[PEER_MPDU_ARR],
		       stats->mpdu[PEER_MPDU_CLONE],
		       stats->mpdu[PEER_MPDU_TO_STACK]);
}
#else

#define TX_CAP_NBUF_QUEUE_FREE(q_head) qdf_nbuf_queue_free(q_head)

/**
 * check_queue_empty() - check queue is empty if not assert
 * @qhead: head of queue
 *
 * Return: void
 */
static inline void check_queue_empty(qdf_nbuf_queue_t *qhead)
{
}

/**
 * dp_tx_cap_stats_msdu_update() - update msdu level stats counter per peer
 * @peer: DP PEER object
 * @msdu_desc: msdu desc index
 * @count: count to update
 *
 * Return: void
 */
static inline
void dp_tx_cap_stats_msdu_update(struct dp_peer *peer,
				 uint8_t msdu_desc, uint32_t count)
{
}

/**
 * dp_tx_cap_stats_mpdu_update() - update mpdu level stats counter per peer
 * @peer: DP PEER object
 * @mpdu_desc: mpdu desc index
 * @count: count to update
 *
 * Return: void
 */
static inline
void dp_tx_cap_stats_mpdu_update(struct dp_peer *peer,
				 uint8_t mpdu_desc, uint32_t count)
{
}

/**
 * dp_tx_capture_print_stats() - print stats counter per peer
 * @peer: DP PEER object
 *
 * Return: void
 */
static inline
void dp_tx_capture_print_stats(struct dp_peer *peer)
{
}
#endif

#define DP_TX_PEER_DEL_REF(peer) \
	dp_tx_peer_del_ref(__func__, __LINE__, peer)

#define DP_TX_PEER_GET_REF(pdev, peer_id) \
	dp_tx_peer_get_ref(__func__, __LINE__, pdev, peer_id)

/**
 * dp_tx_peer_del_ref() - delete the reference held by peer
 * @func: caller function name
 * @line: caller line number
 * @peer: DP PEER object
 *
 * Return: void
 */
static inline
void dp_tx_peer_del_ref(const char *func, uint32_t line, struct dp_peer *peer)
{
	dp_peer_unref_delete(peer, DP_MOD_ID_TX_CAPTURE);
}

/**
 * dp_tx_peer_get_ref() - get the peer reference
 * @func: caller function name
 * @line: caller line number
 * @cur_pdev: DP PDEV object
 * @peer_id: peer id
 *
 * Return: DP PEER object
 */
static inline
struct dp_peer *
dp_tx_peer_get_ref(const char *func, uint32_t line, struct dp_pdev *cur_pdev,
		   uint16_t peer_id)
{
	struct dp_peer *peer = NULL;
	struct dp_vdev *vdev = NULL;
	struct dp_pdev *pdev = NULL;
	struct dp_mon_pdev *cur_mon_pdev = NULL;

	if (qdf_unlikely(!cur_pdev))
		return NULL;

	cur_mon_pdev = cur_pdev->monitor_pdev;

	peer = dp_peer_get_ref_by_id(cur_pdev->soc, peer_id,
				     DP_MOD_ID_TX_CAPTURE);

	if (!peer)
		return NULL;

	/* sanity check vdev NULL */
	vdev = peer->vdev;
	if (qdf_unlikely(!vdev)) {
		DP_TX_PEER_DEL_REF(peer);
		return NULL;
	}

	/* sanity check pdev NULL */
	pdev = vdev->pdev;
	if (qdf_unlikely(!pdev)) {
		DP_TX_PEER_DEL_REF(peer);
		return NULL;
	}

	if (qdf_unlikely(pdev->pdev_id != cur_pdev->pdev_id)) {
		dp_tx_capture_info("%pK: peer %p peer_id: %d mapped to pdev %p %d, cur_pdev %p %d",
				   pdev->soc,
				   peer, peer_id,
				   pdev, pdev->pdev_id,
				   cur_pdev, cur_pdev->pdev_id);
		DP_TX_PEER_DEL_REF(peer);
		cur_mon_pdev->tx_capture.peer_mismatch++;
		return NULL;
	}

	return peer;
}

/*
 * dp_tx_capture_htt_frame_counter: increment counter for htt_frame_type
 * pdev: DP pdev handle
 * htt_frame_type: htt frame type received from fw
 *
 * return: void
 */
void dp_tx_capture_htt_frame_counter(struct dp_pdev *pdev,
				     uint32_t htt_frame_type)
{
	struct dp_mon_pdev *mon_pdev = pdev->monitor_pdev;

	if (htt_frame_type >= TX_CAP_HTT_MAX_FTYPE)
		return;

	mon_pdev->tx_capture.htt_frame_type[htt_frame_type]++;
}

static void
dp_peer_print_tid_qlen(struct dp_soc *soc,
		       struct dp_peer *peer,
		       void *arg)
{
	int tid;
	struct dp_tx_tid *tx_tid;
	uint32_t msdu_len;
	uint32_t tasklet_msdu_len;
	uint32_t ppdu_len;
	struct tid_q_len *c_tid_q_len = (struct tid_q_len *)arg;

	for (tid = 0; tid < DP_MAX_TIDS; tid++) {
		tx_tid = &peer->monitor_peer->tx_capture.tx_tid[tid];
		msdu_len = qdf_nbuf_queue_len(&tx_tid->defer_msdu_q);
		tasklet_msdu_len = qdf_nbuf_queue_len(&tx_tid->msdu_comp_q);
		ppdu_len = qdf_nbuf_queue_len(&tx_tid->pending_ppdu_q);

		/*
		 * if not NULL requested for aggreated stats hence add and
		 * return do not print individual peer stats
		 */
		if (c_tid_q_len) {
			c_tid_q_len->defer_msdu_len += msdu_len;
			c_tid_q_len->tasklet_msdu_len += tasklet_msdu_len;
			c_tid_q_len->pending_q_len += ppdu_len;
			continue;
		}

		if (!msdu_len && !ppdu_len && !tasklet_msdu_len)
			continue;

		DP_PRINT_STATS(" peer_id[%d] tid[%d] msdu_comp_q[%d] defer_msdu_q[%d] pending_ppdu_q[%d] last_deq:%d",
			       peer->peer_id, tid,
			       tasklet_msdu_len,
			       msdu_len, ppdu_len,
			       tx_tid->last_deq_ms);
	}
	dp_tx_capture_print_stats(peer);
}

/*
 * dp_iterate_print_tid_qlen_per_peer()- API to print peer tid msdu queue
 * @pdev_handle: DP_PDEV handle
 *
 * Return: void
 */
void dp_print_tid_qlen_per_peer(void *pdev_hdl, uint8_t consolidated)
{
	struct dp_pdev *pdev = (struct dp_pdev *)pdev_hdl;

	DP_PRINT_STATS("pending peer msdu and ppdu:");
	if (consolidated) {
		struct tid_q_len c_tid_q_len = {0};

		dp_pdev_iterate_peer(pdev, dp_peer_print_tid_qlen, &c_tid_q_len,
				     DP_MOD_ID_TX_CAPTURE);

		DP_PRINT_STATS("consolidated: msdu_comp_q[%llu] defer_msdu_q[%llu] pending_ppdu_q[%llu]",
			       c_tid_q_len.tasklet_msdu_len,
			       c_tid_q_len.defer_msdu_len,
			       c_tid_q_len.pending_q_len);
	}
	dp_pdev_iterate_peer(pdev, dp_peer_print_tid_qlen, NULL,
			     DP_MOD_ID_TX_CAPTURE);
}

static void
dp_ppdu_queue_free(struct dp_pdev *pdev, qdf_nbuf_t ppdu_nbuf, uint8_t usr_idx)
{
	int i;
	struct cdp_tx_completion_ppdu *ppdu_desc = NULL;
	struct cdp_tx_completion_ppdu_user *user;
	qdf_nbuf_t mpdu_nbuf = NULL;
	struct dp_mon_soc *mon_soc = pdev->soc->monitor_soc;

	if (!ppdu_nbuf)
		return;

	ppdu_desc = (struct cdp_tx_completion_ppdu *)qdf_nbuf_data(ppdu_nbuf);
	if (!ppdu_desc)
		return;

	user = &ppdu_desc->user[usr_idx];

	if (!user->mpdus)
		goto free_ppdu_desc_mpdu_q;

	for (i = 0; i < user->ba_size &&
	     i < (CDP_BA_256_BIT_MAP_SIZE_DWORDS * NUM_BITS_DWORD); i++) {
		mpdu_nbuf = user->mpdus[i];
		if (mpdu_nbuf) {
			qdf_nbuf_free(mpdu_nbuf);
			user->mpdus[i] = NULL;
		}
	}

free_ppdu_desc_mpdu_q:

	if (!qdf_nbuf_is_queue_empty(&user->mpdu_q))
		TX_CAP_NBUF_QUEUE_FREE(&user->mpdu_q);

	if (user->mpdus)
		qdf_mem_free(user->mpdus);

	user->mpdus = NULL;

	if (wlan_cfg_get_tx_capt_max_mem(pdev->soc->wlan_cfg_ctx)) {

		/* in case of multiple users or a cloned ppdu,
		 * decrement the single user's mpdu bytes.
		 * else, decrement by user's mpdu bytes and the
		 * descriptor size stored in ppdu_bytes */
		if (qdf_nbuf_is_cloned(ppdu_nbuf) ||
			qdf_nbuf_get_users(ppdu_nbuf) > 1) {
			qdf_atomic_sub(user->mpdu_bytes,
						   &mon_soc->dp_soc_tx_capt.ppdu_bytes);
		} else {
			qdf_atomic_sub(ppdu_desc->ppdu_bytes +
						   user->mpdu_bytes,
						   &mon_soc->dp_soc_tx_capt.ppdu_bytes);
		}
		user->mpdu_bytes = 0;
	}
}

/*
 * dp_print_pdev_tx_capture_stats_1_0: print tx capture stats
 * @pdev: DP PDEV handle
 *
 * return: void
 */
void dp_print_pdev_tx_capture_stats_1_0(struct dp_pdev *pdev)
{
	struct dp_pdev_tx_capture *ptr_tx_cap;
	uint8_t i = 0, j = 0;
	uint32_t ppdu_stats_ms = 0;
	uint32_t now_ms = 0;
	struct dp_mon_pdev *mon_pdev = pdev->monitor_pdev;
	struct dp_mon_soc *mon_soc = pdev->soc->monitor_soc;

	ptr_tx_cap = &mon_pdev->tx_capture;

	ppdu_stats_ms = ptr_tx_cap->ppdu_stats_ms;
	now_ms = qdf_system_ticks_to_msecs(qdf_system_ticks());

	DP_PRINT_STATS("\n\tTX Capture stats:");
	DP_PRINT_STATS(" Last recv ppdu stats: %u",
		       now_ms - ppdu_stats_ms);
	DP_PRINT_STATS(" ppdu stats queue depth: %u",
		       ptr_tx_cap->ppdu_stats_queue_depth);
	DP_PRINT_STATS(" ppdu stats defer queue depth: %u",
		       ptr_tx_cap->ppdu_stats_defer_queue_depth);
	DP_PRINT_STATS(" pending ppdu dropped: %u",
		       ptr_tx_cap->pend_ppdu_dropped);
	DP_PRINT_STATS(" peer mismatch: %llu",
		       ptr_tx_cap->peer_mismatch);
	DP_PRINT_STATS(" ppdu peer flush counter: %u",
		       ptr_tx_cap->ppdu_flush_count);
	DP_PRINT_STATS(" ppdu msdu threshold drop: %u",
		       ptr_tx_cap->msdu_threshold_drop);
	DP_PRINT_STATS(" mgmt control enqueue stats:");
	for (i = 0; i < TXCAP_MAX_TYPE; i++) {
		for (j = 0; j < TXCAP_MAX_SUBTYPE; j++) {
			if (ptr_tx_cap->ctl_mgmt_q[i][j].qlen)
				DP_PRINT_STATS(" ctl_mgmt_q[%d][%d] = queue_len[%d]",
					       i, j,
					       ptr_tx_cap->ctl_mgmt_q[i][j].qlen);
		}
	}
	DP_PRINT_STATS(" mgmt control retry queue stats:");
	for (i = 0; i < TXCAP_MAX_TYPE; i++) {
		for (j = 0; j < TXCAP_MAX_SUBTYPE; j++) {
			if (ptr_tx_cap->retries_ctl_mgmt_q[i][j].qlen)
				DP_PRINT_STATS(" retries_ctl_mgmt_q[%d][%d] = queue_len[%d]",
					       i, j,
					       ptr_tx_cap->retries_ctl_mgmt_q[i][j].qlen);
		}
	}

	for (i = 0; i < TX_CAP_HTT_MAX_FTYPE; i++) {
		if (!ptr_tx_cap->htt_frame_type[i])
			continue;
		DP_PRINT_STATS(" sgen htt frame type[%d] = %d",
			       i, ptr_tx_cap->htt_frame_type[i]);
	}

	dp_print_tid_qlen_per_peer(pdev, 0);

	if (wlan_cfg_get_tx_capt_max_mem(pdev->soc->wlan_cfg_ctx)) {
		DP_PRINT_STATS("mem limit: %u",
			       wlan_cfg_get_tx_capt_max_mem(pdev->soc->wlan_cfg_ctx));
		DP_PRINT_STATS(" DATA ppdu bytes used: %u",
					   qdf_atomic_read(&mon_soc->dp_soc_tx_capt.ppdu_bytes));
		DP_PRINT_STATS(" MGMT ppdu bytes used: %u",
					   qdf_atomic_read(&mon_soc->dp_soc_tx_capt.ppdu_mgmt_bytes));
		DP_PRINT_STATS(" TOTAL ppdu bytes used: %u",
					   qdf_atomic_read(&mon_soc->dp_soc_tx_capt.ppdu_bytes) +
					   qdf_atomic_read(&mon_soc->dp_soc_tx_capt.ppdu_mgmt_bytes));
		DP_PRINT_STATS(" mem limit drops: %u",
					   mon_soc->dp_soc_tx_capt.mem_limit_drops);
		DP_PRINT_STATS(" data enq drops: %u",
					   mon_soc->dp_soc_tx_capt.data_enq_drops);
	}
}

/**
 * dp_peer_or_pdev_tx_cap_enabled - Returns status of tx_cap_enabled
 * based on global per-pdev setting or per-peer setting
 * @pdev: Datapath pdev handle
 * @peer: Datapath peer
 * @mac_addr: peer mac address
 *
 * Return: true if feature is enabled on a per-pdev basis or if
 * enabled for the given peer when per-peer mode is set, false otherwise
 */
inline bool
dp_peer_or_pdev_tx_cap_enabled(struct dp_pdev *pdev,
			       struct dp_peer *peer, uint8_t *mac_addr)
{
	bool flag = false;
	struct dp_mon_pdev *mon_pdev = pdev->monitor_pdev;

	if (mon_pdev->tx_capture_enabled ==
	    CDP_TX_ENH_CAPTURE_ENABLE_ALL_PEERS) {
		return true;
	} else if (mon_pdev->tx_capture_enabled ==
		   CDP_TX_ENH_CAPTURE_ENDIS_PER_PEER) {
		if (peer && (peer->monitor_peer) &&
		    (peer->monitor_peer->tx_cap_enabled))
			return true;

		/* do search based on mac address */
		flag = is_dp_peer_mgmt_pkt_filter(pdev,
						  HTT_INVALID_PEER,
						  mac_addr);
		if (flag && peer && peer->monitor_peer)
			peer->monitor_peer->tx_cap_enabled = 1;

		return flag;
	}
	return false;
}

/*
 * dp_tx_find_usr_idx_from_peer_id()- find user index based on peer_id
 * @ppdu_desc: pointer to ppdu_desc structure
 * @peer_id: peer id
 *
 * Return: user index
 */
static uint8_t
dp_tx_find_usr_idx_from_peer_id(struct cdp_tx_completion_ppdu *ppdu_desc,
				uint16_t peer_id)
{
	uint8_t usr_idx = 0;
	bool found = false;

	for (usr_idx = 0; usr_idx < ppdu_desc->num_users; usr_idx++) {
		if (ppdu_desc->user[usr_idx].peer_id == peer_id) {
			found = true;
			break;
		}
	}

	if (!found) {
		dp_tx_capture_alert("peer_id: %d, ppdu_desc[%p][num_users: %d]\n",
				    peer_id, ppdu_desc, ppdu_desc->num_users);

		for (usr_idx = 0; usr_idx < ppdu_desc->num_users; usr_idx++) {
			dp_tx_capture_alert("peer_id:%d pid:%d sched_cmdid: %d",
					    ppdu_desc->user[usr_idx].peer_id,
					    ppdu_desc->ppdu_id,
					    ppdu_desc->sched_cmdid);
		}

		qdf_assert_always(0);
	}

	return usr_idx;
}

/*
 * dp_peer_tid_peer_id_update_li() – update peer_id to tid structure
 * @peer: Datapath peer
 * @peer_id: peer_id
 *
 */
void dp_peer_tid_peer_id_update_1_0(struct dp_peer *peer, uint16_t peer_id)
{
	int tid;
	struct dp_tx_tid *tx_tid;

	if (!peer || !peer->monitor_peer)
		return;
	/*
	 * For the newly created peer after tx monitor turned ON,
	 * initialization check is already taken care in queue init
	 */
	dp_peer_tid_queue_init(peer);
	for (tid = 0; tid < DP_MAX_TIDS; tid++) {
		tx_tid = &peer->monitor_peer->tx_capture.tx_tid[tid];
		tx_tid->peer_id = peer_id;
		tx_tid->tid = tid;
	}
}

/*
 * dp_peer_tid_queue_init() – Initialize ppdu stats queue per TID
 * @peer: Datapath peer
 *
 */
void dp_peer_tid_queue_init(struct dp_peer *peer)
{
	struct dp_pdev *pdev;
	struct dp_vdev *vdev;
	int tid;
	struct dp_tx_tid *tx_tid;
	struct dp_mon_pdev *mon_pdev;
	struct dp_mon_peer *mon_peer;

	if (!peer)
		return;

	vdev = peer->vdev;
	pdev = vdev->pdev;

	mon_pdev = pdev->monitor_pdev;
	mon_peer = peer->monitor_peer;

	/* only if tx capture is turned on we will initialize the tid */
	if (qdf_atomic_read(&mon_pdev->tx_capture.tx_cap_usr_mode) ==
	    CDP_TX_ENH_CAPTURE_DISABLED)
		return;

	dp_tx_capture_info("%pK: peer(%p) id:%d init!!",
			   pdev->soc, peer, peer->peer_id);

	for (tid = 0; tid < DP_MAX_TIDS; tid++) {
		struct cdp_tx_completion_ppdu *xretry_ppdu = NULL;
		struct cdp_tx_completion_ppdu_user *xretry_user = NULL;

		tx_tid = &mon_peer->tx_capture.tx_tid[tid];

		if (qdf_atomic_test_and_set_bit(DP_PEER_TX_TID_INIT_DONE_BIT,
						&tx_tid->tid_flags))
			continue;

		tx_tid->peer_id = peer->peer_id;
		tx_tid->tid = tid;

		check_queue_empty(&tx_tid->defer_msdu_q);
		qdf_nbuf_queue_init(&tx_tid->defer_msdu_q);
		check_queue_empty(&tx_tid->msdu_comp_q);
		qdf_nbuf_queue_init(&tx_tid->msdu_comp_q);
		check_queue_empty(&tx_tid->pending_ppdu_q);
		qdf_nbuf_queue_init(&tx_tid->pending_ppdu_q);

		tx_tid->max_ppdu_id = 0;

		tx_tid->xretry_ppdu =
			qdf_mem_malloc(sizeof(struct cdp_tx_completion_ppdu) +
				sizeof(struct cdp_tx_completion_ppdu_user));
		if (qdf_unlikely(!tx_tid->xretry_ppdu)) {
			int i;

			dp_tx_capture_err("Alloc failed");
			for (i = 0; i < tid; i++) {
				tx_tid = &mon_peer->tx_capture.tx_tid[i];
				qdf_mem_free(tx_tid->xretry_ppdu);
				tx_tid->xretry_ppdu = NULL;
				qdf_atomic_clear_bit(DP_PEER_TX_TID_INIT_DONE_BIT,
						     &tx_tid->tid_flags);
			}
			QDF_ASSERT(0);
			return;
		}
		/* initialize xretry user mpduq */
		xretry_ppdu = tx_tid->xretry_ppdu;
		xretry_user = &xretry_ppdu->user[0];
		qdf_nbuf_queue_init(&xretry_user->mpdu_q);

		/* spinlock create */
		qdf_spinlock_create(&tx_tid->tid_lock);
		qdf_spinlock_create(&tx_tid->tasklet_tid_lock);
		qdf_atomic_init(&tx_tid->msdu_comp_bytes);
	}

	mon_peer->tx_capture.is_tid_initialized = 1;
}

/*
 * dp_peer_tx_cap_tid_queue_flush() – flush inactive peer tx cap per
 * @soc: Datapath SoC object
 * @peer: Datapath peer
 * @arg: argument
 *
 * return: void
 */
static
void dp_peer_tx_cap_tid_queue_flush(struct dp_soc *soc, struct dp_peer *peer,
				    void *arg)
{
	struct dp_pdev *pdev = NULL;
	struct dp_vdev *vdev = NULL;
	struct dp_tx_tid *tx_tid = NULL;
	struct dp_pdev_flush *flush = (struct dp_pdev_flush *)arg;
	uint16_t peer_id = 0;
	int tid;
	struct dp_mon_peer *mon_peer = peer->monitor_peer;

	/* sanity check vdev NULL */
	vdev = peer->vdev;
	if (qdf_unlikely(!vdev))
		return;

	/* sanity check pdev NULL */
	pdev = vdev->pdev;
	if (qdf_unlikely(!pdev))
		return;

	if (!dp_peer_or_pdev_tx_cap_enabled(pdev, peer, peer->mac_addr.raw))
		return;

	if (!mon_peer->tx_capture.is_tid_initialized)
		return;

	for (tid = 0; tid < DP_MAX_TIDS; tid++) {
		struct cdp_tx_completion_ppdu *xretry_ppdu;
		struct cdp_tx_completion_ppdu_user *xretry_user;
		struct cdp_tx_completion_ppdu *ppdu_desc;
		struct cdp_tx_completion_ppdu_user *user;
		qdf_nbuf_t ppdu_nbuf = NULL;
		uint32_t len = 0;
		uint32_t actual_len = 0;
		uint32_t delta_ms = 0;
		uint32_t nbytes = 0;
		struct dp_mon_soc *mon_soc = soc->monitor_soc;

		tx_tid = &mon_peer->tx_capture.tx_tid[tid];

		/*
		 * check whether the peer tid payload queue is inactive
		 * now_ms will be always greater than last_deq_ms
		 * if it is less than last_deq_ms then wrap
		 * around happened
		 */
		if (flush->now_ms > tx_tid->last_deq_ms)
			delta_ms = flush->now_ms - tx_tid->last_deq_ms;
		else
			delta_ms = (DP_TX_CAP_MAX_MS + flush->now_ms) -
					tx_tid->last_deq_ms;

		/* check whether we need to flush all peer */
		if (!flush->flush_all && delta_ms < TX_CAPTURE_PEER_DEQ_MS)
			continue;

		xretry_ppdu = tx_tid->xretry_ppdu;
		if (qdf_unlikely(!xretry_ppdu))
			continue;

		xretry_user = &xretry_ppdu->user[0];

		qdf_spin_lock_bh(&tx_tid->tid_lock);
		if (wlan_cfg_get_tx_capt_max_mem(pdev->soc->wlan_cfg_ctx) &&
		    !qdf_nbuf_is_queue_empty(&tx_tid->defer_msdu_q)) {
			nbytes = get_queue_bytes(&tx_tid->defer_msdu_q);
			qdf_atomic_sub(nbytes, &mon_soc->dp_soc_tx_capt.ppdu_bytes);
		}
		TX_CAP_NBUF_QUEUE_FREE(&tx_tid->defer_msdu_q);
		/* update last dequeue time with current msec */
		tx_tid->last_deq_ms = flush->now_ms;
		/* update last processed time with current msec */
		tx_tid->last_processed_ms = flush->now_ms;
		qdf_spin_unlock_bh(&tx_tid->tid_lock);

		qdf_spin_lock_bh(&tx_tid->tasklet_tid_lock);
		if (wlan_cfg_get_tx_capt_max_mem(pdev->soc->wlan_cfg_ctx)) {
			qdf_atomic_set(&tx_tid->msdu_comp_bytes, 0);
		}
		TX_CAP_NBUF_QUEUE_FREE(&tx_tid->msdu_comp_q);
		qdf_spin_unlock_bh(&tx_tid->tasklet_tid_lock);

		tx_tid->max_ppdu_id = 0;
		peer_id = tx_tid->peer_id;

		len = qdf_nbuf_queue_len(&tx_tid->pending_ppdu_q);
		actual_len = len;
		/* free pending ppdu_q and xretry mpdu_q */
		while ((ppdu_nbuf = qdf_nbuf_queue_remove(
						&tx_tid->pending_ppdu_q))) {
			uint64_t ppdu_start_timestamp;
			uint8_t usr_idx = 0;

			ppdu_desc = (struct cdp_tx_completion_ppdu *)
					qdf_nbuf_data(ppdu_nbuf);

			ppdu_start_timestamp = ppdu_desc->ppdu_start_timestamp;
			dp_tx_capture_info("ppdu_id:%u tsf:%llu removed from pending ppdu q",
					   ppdu_desc->ppdu_id,
					   ppdu_start_timestamp);
			/*
			 * check if peer id is matching
			 * the user peer_id
			 */
			usr_idx = dp_tx_find_usr_idx_from_peer_id(ppdu_desc,
								  peer_id);
			user = &ppdu_desc->user[usr_idx];

			/* free all the mpdu_q and mpdus for usr_idx */
			dp_ppdu_queue_free(pdev, ppdu_nbuf, usr_idx);
			qdf_nbuf_free(ppdu_nbuf);
			len--;
		}

		if (len) {
			dp_tx_capture_alert("Actual_len: %d pending len:%d !!!",
					    actual_len, len);
			QDF_BUG(0);
		}

		/* free xretry ppdu user alone */
		TX_CAP_NBUF_QUEUE_FREE(&xretry_user->mpdu_q);
	}
}

/*
 * dp_peer_tid_queue_cleanup() – remove ppdu stats queue per TID
 * @peer: Datapath peer
 *
 */
void dp_peer_tid_queue_cleanup(struct dp_peer *peer)
{
	struct dp_tx_tid *tx_tid;
	struct cdp_tx_completion_ppdu *xretry_ppdu;
	struct cdp_tx_completion_ppdu_user *xretry_user;
	struct cdp_tx_completion_ppdu *ppdu_desc;
	struct cdp_tx_completion_ppdu_user *user;
	qdf_nbuf_t ppdu_nbuf = NULL;
	int tid;
	uint16_t peer_id;
	struct dp_mon_peer *mon_peer = peer->monitor_peer;
	struct dp_pdev *pdev = NULL;
	struct dp_mon_soc *mon_soc = NULL;

	if (!mon_peer->tx_capture.is_tid_initialized)
		return;

	pdev = peer->vdev->pdev;
	if (qdf_unlikely(!pdev)) {
		QDF_ASSERT(pdev);
		return;
	}
	mon_soc = pdev->soc->monitor_soc;
	if (qdf_unlikely(!mon_soc)) {
		QDF_ASSERT(mon_soc);
		return;
	}

	dp_tx_capture_info("peer(%p) id:%d cleanup!!",
			   peer, peer->peer_id);

	for (tid = 0; tid < DP_MAX_TIDS; tid++) {
		uint32_t len = 0;
		uint32_t actual_len = 0;

		tx_tid = &mon_peer->tx_capture.tx_tid[tid];

		if (!qdf_atomic_test_and_clear_bit(DP_PEER_TX_TID_INIT_DONE_BIT,
						   &tx_tid->tid_flags))
			continue;

		xretry_ppdu = tx_tid->xretry_ppdu;
		xretry_user = &xretry_ppdu->user[0];

		qdf_spin_lock_bh(&tx_tid->tid_lock);
		len = qdf_nbuf_queue_len(&tx_tid->defer_msdu_q);
		if (wlan_cfg_get_tx_capt_max_mem(pdev->soc->wlan_cfg_ctx) &&
		    !qdf_nbuf_is_queue_empty(&tx_tid->defer_msdu_q)) {
			uint32_t nbytes = get_queue_bytes(&tx_tid->defer_msdu_q);
			qdf_atomic_sub(nbytes, &mon_soc->dp_soc_tx_capt.ppdu_bytes);
		}
		TX_CAP_NBUF_QUEUE_FREE(&tx_tid->defer_msdu_q);
		qdf_spin_unlock_bh(&tx_tid->tid_lock);

		qdf_spin_lock_bh(&tx_tid->tasklet_tid_lock);
		len = qdf_nbuf_queue_len(&tx_tid->msdu_comp_q);
		if (wlan_cfg_get_tx_capt_max_mem(pdev->soc->wlan_cfg_ctx)) {
			qdf_atomic_set(&tx_tid->msdu_comp_bytes, 0);
		}
		TX_CAP_NBUF_QUEUE_FREE(&tx_tid->msdu_comp_q);
		qdf_spin_unlock_bh(&tx_tid->tasklet_tid_lock);

		/* spinlock destroy */
		qdf_spinlock_destroy(&tx_tid->tid_lock);
		qdf_spinlock_destroy(&tx_tid->tasklet_tid_lock);

		peer_id = tx_tid->peer_id;

		len = qdf_nbuf_queue_len(&tx_tid->pending_ppdu_q);
		actual_len = len;

		/* free pending ppdu_q and xretry mpdu_q */
		while ((ppdu_nbuf = qdf_nbuf_queue_remove(
						&tx_tid->pending_ppdu_q))) {
			uint8_t usr_idx;

			ppdu_desc = (struct cdp_tx_completion_ppdu *)
					qdf_nbuf_data(ppdu_nbuf);

			dp_tx_capture_info("ppdu_id:%u tsf:%llu removed from pending ppdu q",
					   ppdu_desc->ppdu_id,
					   ppdu_desc->ppdu_start_timestamp);
			/*
			 * check if peer id is matching
			 * the user peer_id
			 */
			usr_idx = dp_tx_find_usr_idx_from_peer_id(ppdu_desc,
								  peer_id);
			user = &ppdu_desc->user[usr_idx];
			/* free all the mpdu_q and mpdus for usr_idx */
			dp_ppdu_queue_free(pdev, ppdu_nbuf, usr_idx);
			qdf_nbuf_free(ppdu_nbuf);
			len--;
		}

		if (len) {
			dp_tx_capture_alert("Actual_len: %d pending len:%d !!!",
					    actual_len, len);
			QDF_BUG(0);
		}

		TX_CAP_NBUF_QUEUE_FREE(&xretry_user->mpdu_q);
		qdf_mem_free(xretry_ppdu);
		tx_tid->xretry_ppdu = NULL;

		tx_tid->max_ppdu_id = 0;
	}

	mon_peer->tx_capture.is_tid_initialized = 0;
}

/*
 * dp_peer_update_80211_hdr: update 80211 hdr
 * @vdev: DP VDEV
 * @peer: DP PEER
 *
 * return: void
 */
void dp_peer_update_80211_hdr(struct dp_vdev *vdev, struct dp_peer *peer)
{
	struct ieee80211_frame *ptr_wh;

	ptr_wh = &peer->monitor_peer->tx_capture.tx_wifi_hdr;

	/* i_addr1 - Receiver mac address */
	/* i_addr2 - Transmitter mac address */
	/* i_addr3 - Destination mac address */

	qdf_mem_copy(ptr_wh->i_addr1,
		     peer->mac_addr.raw,
		     QDF_MAC_ADDR_SIZE);
	qdf_mem_copy(ptr_wh->i_addr3,
		     peer->mac_addr.raw,
		     QDF_MAC_ADDR_SIZE);
	qdf_mem_copy(ptr_wh->i_addr2,
		     vdev->mac_addr.raw,
		     QDF_MAC_ADDR_SIZE);
}

/*
 * dp_action_frame_is_hostgen
 * @category: action category code number
 * @action: action code number
 *
 * return: true if action frame is host generated, else false
 */
bool dp_action_frame_is_hostgen(int category, int action)
{
	bool retval = true;

	switch (category) {
	case IEEE80211_ACTION_CAT_BA:
		switch (action) {
		case IEEE80211_ACTION_BA_ADDBA_REQUEST:
		case IEEE80211_ACTION_BA_ADDBA_RESPONSE:
		case IEEE80211_ACTION_BA_DELBA:
			retval = false;
			break;
		default:
			break;
		}
	break;

	case IEEE80211_ACTION_CAT_PUBLIC:
		switch (action) {
		case IEEE80211_ACTION_PUBLIC_FINE_TMR:
		case IEEE80211_ACTION_PUBLIC_FINE_TM:
		case IEEE80211_ACTION_PUBLIC_FILS_DISC:
			retval = false;
			break;
		default:
			break;
		}
	break;

	case IEEE80211_ACTION_CAT_RADIO:
		switch (action) {
		case IEEE80211_ACTION_RADIO_MGMT_NEIGH_REPT_REQ:
			retval = false;
			break;
		default:
			break;
		}
	break;

	case IEEE80211_ACTION_CAT_HT:
		switch (action) {
		case IEEE80211_ACTION_HT_SMPOWERSAVE:
			retval = false;
			break;
		default:
			break;
		}
	break;

	case IEEE80211_ACTION_CAT_SA_QUERY:
		switch (action) {
		case IEEE80211_ACTION_SA_QUERY_REQUEST:
		case IEEE80211_ACTION_SA_QUERY_RESPONSE:
			retval = false;
			break;
		default:
			break;
		}
	break;

	case IEEE80211_ACTION_CAT_WNM:
		switch (action) {
		case IEEE80211_ACTION_EVENT_REPORT:
		case IEEE80211_ACTION_BSTM_QUERY:
		case IEEE80211_ACTION_BSTM_RESP:
			retval = false;
			break;
		default:
			break;
		}
	break;

	case IEEE80211_ACTION_CAT_TDLS:
		switch (action) {
		case IEEE80211_ACTION_TDLS_PEER_TRAFFIC_IND:
		case IEEE80211_ACTION_TDLS_CHAN_SWITCH_REQ:
		case IEEE80211_ACTION_TDLS_CHAN_SWITCH_RESP:
		case IEEE80211_ACTION_TDLS_PEER_TRAFFIC_RESP:
			retval = false;
			break;
		default:
			break;
		}
	break;

	case IEEE80211_ACTION_CAT_VHT:
		switch (action) {
		case IEEE80211_ACTION_VHT_OPMODE:
		case IEEE80211_ACTION_VHT_GROUP_ID:
			retval = false;
			break;
		default:
			break;
		}
	break;

	case IEEE80211_ACTION_CAT_S1G:
		switch (action) {
		case IEEE80211_ACTION_TWT_SETUP:
		case IEEE80211_ACTION_TWT_TEARDOWN:
		case IEEE80211_ACTION_TWT_INFORMATION:
			retval = false;
			break;
		default:
			break;
		}
	break;

	default:
		break;
	}
	return retval;
}

/*
 * dp_deliver_mgmt_frm: Process
 * @pdev: DP PDEV handle
 * @nbuf: buffer containing the htt_ppdu_stats_tx_mgmtctrl_payload_tlv
 *
 * return: void
 */
void dp_deliver_mgmt_frm(struct dp_pdev *pdev, qdf_nbuf_t nbuf)
{
	struct dp_mon_pdev *mon_pdev = pdev->monitor_pdev;
	struct dp_mon_soc *mon_soc = pdev->soc->monitor_soc;

	if (mon_pdev->tx_sniffer_enable || mon_pdev->mcopy_mode) {
		dp_wdi_event_handler(WDI_EVENT_TX_MGMT_CTRL, pdev->soc,
				     nbuf, HTT_INVALID_PEER,
				     WDI_NO_VAL, pdev->pdev_id);
		return;
	}
	if ((mon_pdev->tx_capture_enabled ==
	     CDP_TX_ENH_CAPTURE_ENABLE_ALL_PEERS) ||
	    (mon_pdev->tx_capture_enabled ==
	     CDP_TX_ENH_CAPTURE_ENDIS_PER_PEER)) {
		/* invoke WDI event handler here send mgmt pkt here */
		struct ieee80211_frame *wh;
		uint8_t type, subtype;
		struct cdp_tx_mgmt_comp_info *ptr_mgmt_hdr;

		ptr_mgmt_hdr = (struct cdp_tx_mgmt_comp_info *)
				qdf_nbuf_data(nbuf);
		wh = (struct ieee80211_frame *)(qdf_nbuf_data(nbuf) +
			sizeof(struct cdp_tx_mgmt_comp_info));
		type = (wh->i_fc[0] & IEEE80211_FC0_TYPE_MASK) >>
			IEEE80211_FC0_TYPE_SHIFT;
		subtype = (wh->i_fc[0] & IEEE80211_FC0_SUBTYPE_MASK) >>
			IEEE80211_FC0_SUBTYPE_SHIFT;

		if (!ptr_mgmt_hdr->ppdu_id || !ptr_mgmt_hdr->tx_tsf ||
		    (!type && !subtype)) {
			/*
			 * if either ppdu_id and tx_tsf are zero then
			 * storing the payload won't be useful
			 * in constructing the packet
			 * Hence freeing the packet
			 */
			qdf_nbuf_free(nbuf);
			return;
		}

		if (!dp_peer_or_pdev_tx_cap_enabled(pdev, NULL, wh->i_addr1)) {
			qdf_nbuf_free(nbuf);
			return;
		}

		if (((wh->i_fc[0] & IEEE80211_FC0_TYPE_MASK) ==
		    IEEE80211_FC0_TYPE_MGT) &&
		    ((wh->i_fc[0] & IEEE80211_FC0_SUBTYPE_MASK) ==
		     IEEE80211_FC0_SUBTYPE_ACTION)) {
			uint8_t *frm;
			struct ieee80211_action *ia;

			frm = (u_int8_t *)&wh[1];
			ia = (struct ieee80211_action *)frm;

			if (!dp_action_frame_is_hostgen(ia->ia_category,
							ia->ia_action)) {
				ptr_mgmt_hdr->is_sgen_pkt = false;
			}
		}

		/* if adding the new buffer goes beyond the allowed limit,
		 * drop the buffer */
		if (wlan_cfg_get_tx_capt_max_mem(pdev->soc->wlan_cfg_ctx)) {
			if (!dp_tx_capt_mem_check(pdev, qdf_nbuf_get_truesize(nbuf))) {
				qdf_nbuf_free(nbuf);
				mon_soc->dp_soc_tx_capt.mem_limit_drops++;
				return;
			} else {
				qdf_atomic_add(qdf_nbuf_get_truesize(nbuf),
					       &mon_soc->dp_soc_tx_capt.ppdu_mgmt_bytes);
			}
		}

		dp_tx_capture_debug("%pK: dlvr mgmt frm(%d 0x%08x): fc 0x%x %x, dur 0x%x%x tsf:%llu, retries_count: %d, is_sgen: %d",
				    pdev->soc,
				    ptr_mgmt_hdr->ppdu_id,
				    ptr_mgmt_hdr->ppdu_id,
				    wh->i_fc[1], wh->i_fc[0],
				    wh->i_dur[1], wh->i_dur[0],
				    ptr_mgmt_hdr->tx_tsf,
				    ptr_mgmt_hdr->retries_count,
				    ptr_mgmt_hdr->is_sgen_pkt);

		QDF_TRACE_HEX_DUMP(QDF_MODULE_ID_DP_TX_CAPTURE,
				   QDF_TRACE_LEVEL_DEBUG,
				   qdf_nbuf_data(nbuf), 64);

		qdf_spin_lock_bh(
			&mon_pdev->tx_capture.ctl_mgmt_lock[type][subtype]);
		qdf_nbuf_queue_add(
			&mon_pdev->tx_capture.ctl_mgmt_q[type][subtype], nbuf);
		qdf_spin_unlock_bh(
			&mon_pdev->tx_capture.ctl_mgmt_lock[type][subtype]);
	} else {
		if (!mon_pdev->bpr_enable)
			qdf_nbuf_free(nbuf);
	}
}

static inline int dp_peer_compare_mac_addr(void *addr1, void *addr2)
{
	union dp_align_mac_addr *mac_addr1 = (union dp_align_mac_addr *)addr1;
	union dp_align_mac_addr *mac_addr2 = (union dp_align_mac_addr *)addr2;

	return !((mac_addr1->align4.bytes_abcd == mac_addr2->align4.bytes_abcd)
		 & (mac_addr1->align4.bytes_ef == mac_addr2->align4.bytes_ef));
}

/*
 * dp_peer_tx_cap_search: filter mgmt pkt based on peer and mac address
 * @pdev: DP PDEV handle
 * @peer_id: DP PEER ID
 * @mac_addr: pointer to mac address
 *
 * return: true on matched and false on not found
 */
static
bool dp_peer_tx_cap_search(struct dp_pdev *pdev,
			   uint16_t peer_id, uint8_t *mac_addr)
{
	struct dp_pdev_tx_capture *tx_capture;
	struct dp_peer_mgmt_list *ptr_peer_mgmt_list;
	uint8_t i = 0;
	bool found = false;
	struct dp_mon_pdev *mon_pdev = pdev->monitor_pdev;

	tx_capture = &mon_pdev->tx_capture;

	/* search based on mac address */
	for (i = 0; i < MAX_MGMT_PEER_FILTER; i++) {
		uint8_t *peer_mac_addr;

		ptr_peer_mgmt_list = &tx_capture->ptr_peer_mgmt_list[i];
		if (ptr_peer_mgmt_list->avail)
			continue;
		peer_mac_addr = ptr_peer_mgmt_list->mac_addr;
		if (!dp_peer_compare_mac_addr(mac_addr,
					      peer_mac_addr)) {
			found = true;
			break;
		}
	}
	return found;
}

/*
 * dp_peer_tx_cap_add_filter: add peer filter mgmt pkt based on peer
 * and mac address
 * @pdev: DP PDEV handle
 * @peer_id: DP PEER ID
 * @mac_addr: pointer to mac address
 *
 * return: true on added and false on not failed
 */
bool dp_peer_tx_cap_add_filter(struct dp_pdev *pdev,
			       uint16_t peer_id, uint8_t *mac_addr)
{
	struct dp_pdev_tx_capture *tx_capture;
	struct dp_peer_mgmt_list *ptr_peer_mgmt_list;
	uint8_t i = 0;
	bool status = false;
	struct dp_mon_pdev *mon_pdev = pdev->monitor_pdev;

	tx_capture = &mon_pdev->tx_capture;

	if (dp_peer_tx_cap_search(pdev, peer_id, mac_addr)) {
		/* mac address and peer_id already there */
		dp_tx_capture_info("%pk: peer_id[%d] mac_addr[%pM] already there\n",
				   pdev->soc, peer_id, mac_addr);
		return status;
	}

	for (i = 0; i < MAX_MGMT_PEER_FILTER; i++) {
		ptr_peer_mgmt_list = &tx_capture->ptr_peer_mgmt_list[i];
		if (!ptr_peer_mgmt_list->avail)
			continue;
		qdf_mem_copy(ptr_peer_mgmt_list->mac_addr,
			     mac_addr, QDF_MAC_ADDR_SIZE);
		ptr_peer_mgmt_list->avail = false;
		ptr_peer_mgmt_list->peer_id = peer_id;
		status = true;
		break;
	}

	return status;
}

/*
 * dp_peer_tx_cap_del_all_filter: delete all peer filter mgmt pkt based on peer
 * and mac address
 * @pdev: DP PDEV handle
 * @peer_id: DP PEER ID
 * @mac_addr: pointer to mac address
 *
 * return: void
 */
void dp_peer_tx_cap_del_all_filter(struct dp_pdev *pdev)
{
	struct dp_pdev_tx_capture *tx_capture;
	struct dp_peer_mgmt_list *ptr_peer_mgmt_list;
	uint8_t i = 0;
	struct dp_mon_pdev *mon_pdev = pdev->monitor_pdev;

	tx_capture = &mon_pdev->tx_capture;

	for (i = 0; i < MAX_MGMT_PEER_FILTER; i++) {
		ptr_peer_mgmt_list = &tx_capture->ptr_peer_mgmt_list[i];
		ptr_peer_mgmt_list->avail = true;
		ptr_peer_mgmt_list->peer_id = HTT_INVALID_PEER;
		qdf_mem_zero(ptr_peer_mgmt_list->mac_addr, QDF_MAC_ADDR_SIZE);
	}
}

/*
 * dp_peer_tx_cap_del_filter: delete peer filter mgmt pkt based on peer
 * and mac address
 * @pdev: DP PDEV handle
 * @peer_id: DP PEER ID
 * @mac_addr: pointer to mac address
 *
 * return: true on added and false on not failed
 */
bool dp_peer_tx_cap_del_filter(struct dp_pdev *pdev,
			       uint16_t peer_id, uint8_t *mac_addr)
{
	struct dp_pdev_tx_capture *tx_capture;
	struct dp_peer_mgmt_list *ptr_peer_mgmt_list;
	uint8_t i = 0;
	bool status = false;
	struct dp_mon_pdev *mon_pdev = pdev->monitor_pdev;

	tx_capture = &mon_pdev->tx_capture;

	for (i = 0; i < MAX_MGMT_PEER_FILTER; i++) {
		ptr_peer_mgmt_list = &tx_capture->ptr_peer_mgmt_list[i];
		if (!dp_peer_compare_mac_addr(mac_addr,
					      ptr_peer_mgmt_list->mac_addr) &&
		    (!ptr_peer_mgmt_list->avail)) {
			ptr_peer_mgmt_list->avail = true;
			ptr_peer_mgmt_list->peer_id = HTT_INVALID_PEER;
			qdf_mem_zero(ptr_peer_mgmt_list->mac_addr,
				     QDF_MAC_ADDR_SIZE);
			status = true;
			break;
		}
	}

	if (!status)
		dp_tx_capture_info("%pK: unable to delete peer[%d] mac[%pM] filter list",
				   pdev->soc, peer_id, mac_addr);
	return status;
}

/*
 * dp_peer_tx_cap_print_mgmt_filter: pradd peer filter mgmt pkt based on peer
 * and mac address
 * @pdev: DP PDEV handle
 * @peer_id: DP PEER ID
 * @mac_addr: pointer to mac address
 *
 * return: true on added and false on not failed
 */
void dp_peer_tx_cap_print_mgmt_filter(struct dp_pdev *pdev,
				      uint16_t peer_id, uint8_t *mac_addr)
{
	struct dp_pdev_tx_capture *tx_capture;
	struct dp_peer_mgmt_list *ptr_peer_mgmt_list;
	uint8_t i = 0;
	struct dp_mon_pdev *mon_pdev = pdev->monitor_pdev;

	tx_capture = &mon_pdev->tx_capture;

	dp_tx_capture_info("%pK: peer filter list:", pdev->soc);
	for (i = 0; i < MAX_MGMT_PEER_FILTER; i++) {
		ptr_peer_mgmt_list = &tx_capture->ptr_peer_mgmt_list[i];
		dp_tx_capture_info("%pK: peer_id[%d] mac_addr[%pM] avail[%d]",
				   pdev->soc,
				   ptr_peer_mgmt_list->peer_id,
				   ptr_peer_mgmt_list->mac_addr,
				   ptr_peer_mgmt_list->avail);
	}
}

/*
 * dp_peer_mgmt_pkt_filter: filter mgmt pkt based on peer and mac address
 * @pdev: DP PDEV handle
 * @nbuf: buffer containing the ppdu_desc
 *
 * return: status
 */
bool is_dp_peer_mgmt_pkt_filter(struct dp_pdev *pdev,
				uint32_t peer_id, uint8_t *mac_addr)
{
	bool found = false;

	found = dp_peer_tx_cap_search(pdev, peer_id, mac_addr);
	dp_tx_capture_info("%pK: peer_id[%d] mac_addr[%pM] found[%d]!",
			   pdev->soc, peer_id, mac_addr, found);

	return found;
}

/**
 * ppdu_desc_dbg_enqueue() - enqueue tx cap debug ppdu desc to the list
 * @ptr_log_info: tx capture log info
 * @ptr_dbg_ppdu: tx capture ppdu desc pointer
 *
 * Return: list size
 */
static
uint32_t ppdu_desc_dbg_enqueue(struct tx_cap_debug_log_info *ptr_log_info,
			       struct dbg_tx_comp_ppdu *ptr_dbg_ppdu)
{
	uint32_t list_size;

	qdf_spin_lock(&ptr_log_info->dbg_log_lock);
	qdf_list_insert_back_size(&ptr_log_info->ppdu_dbg_queue,
				  &ptr_dbg_ppdu->node, &list_size);
	qdf_spin_unlock(&ptr_log_info->dbg_log_lock);

	return list_size;
}

/**
 * ppdu_desc_dbg_dequeue() - dequeue tx cap debug ppdu desc from list
 * @ptr_log_info: tx capture log info
 *
 * Return: tx capture ppdu desc pointer
 */
static
struct dbg_tx_comp_ppdu *
ppdu_desc_dbg_dequeue(struct tx_cap_debug_log_info *ptr_log_info)
{
	qdf_list_node_t *list_node = NULL;

	qdf_spin_lock(&ptr_log_info->dbg_log_lock);
	qdf_list_remove_front(&ptr_log_info->ppdu_dbg_queue, &list_node);
	qdf_spin_unlock(&ptr_log_info->dbg_log_lock);

	if (!list_node)
		return NULL;

	return qdf_container_of(list_node, struct dbg_tx_comp_ppdu, node);
}

/*
 * ppdu_desc_dbg_queue_init: ppdu desc logging debugfs queue init
 * @ptr_log_info: tx capture debugfs log structure
 *
 * return: void
 */
static void ppdu_desc_dbg_queue_init(struct tx_cap_debug_log_info *ptr_log_info)
{
	ptr_log_info->ppdu_queue_size = MAX_TX_CAP_QUEUE_SIZE;

	qdf_list_create(&ptr_log_info->ppdu_dbg_queue,
			ptr_log_info->ppdu_queue_size);
	qdf_spinlock_create(&ptr_log_info->dbg_log_lock);

	ptr_log_info->pause_dbg_log = 0;
}

/*
 * ppdu_desc_dbg_queue_deinit: ppdu desc logging debugfs queue deinit
 * @ptr_log_info: tx capture debugfs log structure
 *
 * return: void
 */
static
void ppdu_desc_dbg_queue_deinit(struct tx_cap_debug_log_info *ptr_log_info)
{
	qdf_list_destroy(&ptr_log_info->ppdu_dbg_queue);
	qdf_spinlock_destroy(&ptr_log_info->dbg_log_lock);
}

/*
 * dp_tx_capture_work_q_timer_handler()- timer to schedule tx capture
 * work queue
 * @arg: pdev Handle
 *
 * Return:
 *
 */
static void dp_tx_capture_work_q_timer_handler(void *arg)
{
	struct dp_pdev *pdev = (struct dp_pdev *)arg;
	struct dp_mon_pdev *mon_pdev;

	if (!pdev || !pdev->monitor_pdev)
		return;

	mon_pdev = pdev->monitor_pdev;
	qdf_queue_work(0, mon_pdev->tx_capture.ppdu_stats_workqueue,
		       &mon_pdev->tx_capture.ppdu_stats_work);
	if (!mon_pdev->stop_tx_capture_work_q_timer) {
		qdf_timer_mod(&mon_pdev->tx_capture.work_q_timer,
			      TX_CAPTURE_WORK_Q_TIMER_MS);
	}
}

/**
 * dp_tx_ppdu_stats_attach_1_0 - Initialize Tx PPDU stats and enhanced capture
 * @pdev: DP PDEV
 *
 * Return: none
 */
void dp_tx_ppdu_stats_attach_1_0(struct dp_pdev *pdev)
{
	struct dp_peer_mgmt_list *ptr_peer_mgmt_list;
	struct dp_pdev_tx_capture *tx_capture;
	struct tx_cap_debug_log_info *ptr_log_info;
	int i, j;
	struct dp_mon_pdev *mon_pdev = pdev->monitor_pdev;

	qdf_atomic_init(&mon_pdev->tx_capture.tx_cap_usr_mode);
	qdf_atomic_set(&mon_pdev->tx_capture.tx_cap_usr_mode, 0);

	tx_capture = &mon_pdev->tx_capture;
	ptr_log_info = &tx_capture->log_info;

	/* Work queue setup for HTT stats and tx capture handling */
	qdf_create_work(0, &mon_pdev->tx_capture.ppdu_stats_work,
			dp_tx_ppdu_stats_process,
			pdev);
	mon_pdev->tx_capture.ppdu_stats_workqueue =
		qdf_alloc_unbound_workqueue("ppdu_stats_work_queue");
	STAILQ_INIT(&mon_pdev->tx_capture.ppdu_stats_queue);
	STAILQ_INIT(&mon_pdev->tx_capture.ppdu_stats_defer_queue);
	qdf_spinlock_create(&mon_pdev->tx_capture.ppdu_stats_lock);
	mon_pdev->tx_capture.ppdu_stats_queue_depth = 0;
	mon_pdev->tx_capture.ppdu_stats_defer_queue_depth = 0;
	mon_pdev->tx_capture.ppdu_dropped = 0;

	qdf_timer_init(NULL, &mon_pdev->tx_capture.work_q_timer,
		       dp_tx_capture_work_q_timer_handler,
		       (void *)pdev,
		       QDF_TIMER_TYPE_WAKE_APPS);
	mon_pdev->stop_tx_capture_work_q_timer = FALSE;

	for (i = 0; i < TXCAP_MAX_TYPE; i++) {
		for (j = 0; j < TXCAP_MAX_SUBTYPE; j++) {
			check_queue_empty(
				&mon_pdev->tx_capture.ctl_mgmt_q[i][j]);
			qdf_nbuf_queue_init(
				&mon_pdev->tx_capture.ctl_mgmt_q[i][j]);
			qdf_spinlock_create(
				&mon_pdev->tx_capture.ctl_mgmt_lock[i][j]);
			check_queue_empty(
				&mon_pdev->tx_capture.retries_ctl_mgmt_q[i][j]);
			qdf_nbuf_queue_init(
				&mon_pdev->tx_capture.retries_ctl_mgmt_q[i][j]);
		}
	}

	mon_pdev->tx_capture.dummy_ppdu_desc = qdf_mem_malloc(
				 sizeof(struct cdp_tx_completion_ppdu) +
				 sizeof(struct cdp_tx_completion_ppdu_user));

	if (qdf_unlikely(!mon_pdev->tx_capture.dummy_ppdu_desc)) {
		dp_tx_capture_err("%pK: Alloc failed", pdev->soc);
		QDF_ASSERT(0);
		return;
	}

	mon_pdev->tx_capture.ptr_peer_mgmt_list = (struct dp_peer_mgmt_list *)
			qdf_mem_malloc(sizeof(struct dp_peer_mgmt_list) *
				       MAX_MGMT_PEER_FILTER);
	for (i = 0; i < MAX_MGMT_PEER_FILTER; i++) {
		ptr_peer_mgmt_list = &tx_capture->ptr_peer_mgmt_list[i];
		ptr_peer_mgmt_list->avail = true;
	}

	ppdu_desc_dbg_queue_init(ptr_log_info);
}

/**
 * dp_tx_ppdu_stats_detach_1_0 - Cleanup Tx PPDU stats and enhanced capture
 * @pdev: DP PDEV
 *
 * Return: none
 */
void dp_tx_ppdu_stats_detach_1_0(struct dp_pdev *pdev)
{
	struct ppdu_info *ppdu_info, *tmp_ppdu_info = NULL;
	struct tx_cap_debug_log_info *ptr_log_info;
	struct dp_mon_pdev *mon_pdev;
	int i, j;
	void *buf;

	if (!pdev || !pdev->monitor_pdev ||
	    !pdev->monitor_pdev->tx_capture.ppdu_stats_workqueue)
		return;

	mon_pdev = pdev->monitor_pdev;
	ptr_log_info = &mon_pdev->tx_capture.log_info;

	mon_pdev->stop_tx_capture_work_q_timer = TRUE;
	qdf_timer_sync_cancel(&mon_pdev->tx_capture.work_q_timer);
	qdf_timer_free(&mon_pdev->tx_capture.work_q_timer);

	qdf_flush_workqueue(0, mon_pdev->tx_capture.ppdu_stats_workqueue);
	qdf_destroy_workqueue(0, mon_pdev->tx_capture.ppdu_stats_workqueue);

	qdf_spinlock_destroy(&mon_pdev->tx_capture.ppdu_stats_lock);

	STAILQ_FOREACH_SAFE(ppdu_info,
			    &mon_pdev->tx_capture.ppdu_stats_queue,
			    ppdu_info_queue_elem, tmp_ppdu_info) {
		STAILQ_REMOVE(&mon_pdev->tx_capture.ppdu_stats_queue,
			      ppdu_info, ppdu_info, ppdu_info_queue_elem);
		qdf_nbuf_free(ppdu_info->nbuf);
		qdf_mem_free(ppdu_info);
	}

	STAILQ_FOREACH_SAFE(ppdu_info,
			    &mon_pdev->tx_capture.ppdu_stats_defer_queue,
			    ppdu_info_queue_elem, tmp_ppdu_info) {
		STAILQ_REMOVE(&mon_pdev->tx_capture.ppdu_stats_defer_queue,
			      ppdu_info, ppdu_info, ppdu_info_queue_elem);
		qdf_nbuf_free(ppdu_info->nbuf);
		qdf_mem_free(ppdu_info);
	}

	for (i = 0; i < TXCAP_MAX_TYPE; i++) {
		for (j = 0; j < TXCAP_MAX_SUBTYPE; j++) {
			qdf_nbuf_queue_t *retries_q;
			uint32_t nbytes;
			struct dp_mon_soc *mon_soc = pdev->soc->monitor_soc;

			if (wlan_cfg_get_tx_capt_max_mem(pdev->soc->wlan_cfg_ctx)) {
				nbytes = get_queue_bytes(&mon_pdev->tx_capture.ctl_mgmt_q[i][j]);
				qdf_atomic_sub(nbytes, &mon_soc->dp_soc_tx_capt.ppdu_mgmt_bytes);
			}

			qdf_spin_lock_bh(
				&mon_pdev->tx_capture.ctl_mgmt_lock[i][j]);
			TX_CAP_NBUF_QUEUE_FREE(
				&mon_pdev->tx_capture.ctl_mgmt_q[i][j]);
			qdf_spin_unlock_bh(
				&mon_pdev->tx_capture.ctl_mgmt_lock[i][j]);
			qdf_spinlock_destroy(
				&mon_pdev->tx_capture.ctl_mgmt_lock[i][j]);

			retries_q =
				&mon_pdev->tx_capture.retries_ctl_mgmt_q[i][j];

			if (!qdf_nbuf_is_queue_empty(retries_q)) {
				if (wlan_cfg_get_tx_capt_max_mem(pdev->soc->wlan_cfg_ctx)) {
					uint32_t nbytes = get_queue_bytes(retries_q);
					qdf_atomic_sub(nbytes, &mon_soc->dp_soc_tx_capt.ppdu_mgmt_bytes);
				}
				TX_CAP_NBUF_QUEUE_FREE(retries_q);
			}
		}
	}

	qdf_mem_free(mon_pdev->tx_capture.dummy_ppdu_desc);
	qdf_mem_free(mon_pdev->tx_capture.ptr_peer_mgmt_list);

	/* disable the ppdu_desc_log to avoid storing further */
	ptr_log_info->ppdu_desc_log = 0;
	ptr_log_info->pause_dbg_log = 1;

	/* loop through the list and free the nbuf */
	while (!qdf_list_empty(&ptr_log_info->ppdu_dbg_queue)) {
		buf = ppdu_desc_dbg_dequeue(ptr_log_info);
		if (!buf)
			qdf_mem_free(buf);
	}

	ppdu_desc_dbg_queue_deinit(ptr_log_info);

	dp_tx_capture_debugfs_deinit_1_0(pdev);
}

#define MAX_MSDU_THRESHOLD_TSF 500000
#define MAX_MSDU_ENQUEUE_THRESHOLD 4096

/**
 * dp_drop_enq_msdu_on_thresh(): Function to drop msdu when exceed
 * storing threshold limit
 * @pdev: Datapath pdev object
 * @peer: Datapath peer object
 * @tx_tid: tx tid
 * @ptr_msdu_comp_q: pointer to skb queue, it can be either tasklet or WQ msdu q
 * @tsf: current timestamp
 *
 * return: status
 */
QDF_STATUS
dp_drop_enq_msdu_on_thresh(struct dp_pdev *pdev,
			   struct dp_peer *peer,
			   struct dp_tx_tid *tx_tid,
			   qdf_nbuf_queue_t *ptr_msdu_comp_q,
			   uint32_t tsf)
{
	struct msdu_completion_info *ptr_msdu_info = NULL;
	struct dp_mon_pdev *mon_pdev = pdev->monitor_pdev;
	qdf_nbuf_t nbuf;
	qdf_nbuf_t head_msdu;
	uint32_t tsf_delta;
	uint32_t comp_qlen = 0;
	uint32_t defer_msdu_qlen = 0;
	uint32_t qlen = 0;
	uint32_t ppdu_id = 0;
	uint32_t cur_ppdu_id = 0;

	/* take lock here */
	qdf_spin_lock_bh(&tx_tid->tasklet_tid_lock);
	while ((head_msdu = qdf_nbuf_queue_first(ptr_msdu_comp_q))) {
		ptr_msdu_info =
		(struct msdu_completion_info *)qdf_nbuf_data(head_msdu);

		if (tsf > ptr_msdu_info->tsf)
			tsf_delta = tsf - ptr_msdu_info->tsf;
		else
			tsf_delta = LOWER_32_MASK - ptr_msdu_info->tsf + tsf;

		/* exit loop if time difference is less than 100 msec */
		if (tsf_delta < MAX_MSDU_THRESHOLD_TSF)
			break;

		/* free head */
		nbuf = qdf_nbuf_queue_remove(ptr_msdu_comp_q);
		if (qdf_unlikely(!nbuf)) {
			qdf_assert_always(0);
			break;
		}

		if (wlan_cfg_get_tx_capt_max_mem(pdev->soc->wlan_cfg_ctx)) {
			qdf_atomic_sub(qdf_nbuf_get_truesize(nbuf),
				       &tx_tid->msdu_comp_bytes);
		}
		qdf_nbuf_free(nbuf);
		dp_tx_cap_stats_msdu_update(peer, PEER_MSDU_DROP, 1);
		mon_pdev->tx_capture.msdu_threshold_drop++;
	}

	/* get queue length */
	comp_qlen = qdf_nbuf_queue_len(ptr_msdu_comp_q);
	/* release lock here */
	qdf_spin_unlock_bh(&tx_tid->tasklet_tid_lock);

	/* drop based on queue length is done when mem limit feature
	 * is NOT enabled */
	if (!wlan_cfg_get_tx_capt_max_mem(pdev->soc->wlan_cfg_ctx)) {
		/* take lock here */
		qdf_spin_lock_bh(&tx_tid->tid_lock);
		defer_msdu_qlen = qdf_nbuf_queue_len(&tx_tid->defer_msdu_q);
		qlen = defer_msdu_qlen + comp_qlen;

		/* Drop queued msdu when exceed threshold */
		while (qlen > MAX_MSDU_ENQUEUE_THRESHOLD) {
			/* get first msdu */
			head_msdu = qdf_nbuf_queue_first(&tx_tid->defer_msdu_q);
			if (qdf_unlikely(!head_msdu))
				break;
			ptr_msdu_info =
				(struct msdu_completion_info *)qdf_nbuf_data(head_msdu);
			ppdu_id = ptr_msdu_info->ppdu_id;
			cur_ppdu_id = ppdu_id;
			while (ppdu_id == cur_ppdu_id) {
				qdf_nbuf_t nbuf = NULL;

				/* free head, nbuf will be NULL if queue empty */
				nbuf = qdf_nbuf_queue_remove(&tx_tid->defer_msdu_q);
				if (qdf_likely(nbuf)) {
					qdf_nbuf_free(nbuf);
					dp_tx_cap_stats_msdu_update(peer,
								    PEER_MSDU_DROP, 1);
					mon_pdev->tx_capture.msdu_threshold_drop++;
				}

				head_msdu = qdf_nbuf_queue_first(&tx_tid->defer_msdu_q);
				if (qdf_unlikely(!head_msdu))
					break;
				ptr_msdu_info =
					(struct msdu_completion_info *)qdf_nbuf_data(head_msdu);
				cur_ppdu_id = ptr_msdu_info->ppdu_id;
			}
			/* get length again */
			defer_msdu_qlen = qdf_nbuf_queue_len(&tx_tid->defer_msdu_q);
			qlen = defer_msdu_qlen + comp_qlen;
		}
		/* release lock here */
		qdf_spin_unlock_bh(&tx_tid->tid_lock);
	}

	return QDF_STATUS_SUCCESS;
}

/**
 * dp_update_msdu_to_list(): Function to queue msdu from wbm
 * @pdev: dp_pdev
 * @peer: dp_peer
 * @ts: hal tx completion status
 * @netbuf: msdu
 *
 * return: status
 */
QDF_STATUS
dp_update_msdu_to_list(struct dp_soc *soc,
		       struct dp_pdev *pdev,
		       struct dp_peer *peer,
		       struct hal_tx_completion_status *ts,
		       qdf_nbuf_t netbuf)
{
	struct dp_tx_tid *tx_tid;
	struct msdu_completion_info *msdu_comp_info;
	struct dp_mon_pdev *mon_pdev = pdev->monitor_pdev;
	struct dp_mon_soc *mon_soc = pdev->soc->monitor_soc;

	if (!peer) {
		dp_tx_capture_err("%pK: peer NULL !", soc);
		return QDF_STATUS_E_FAILURE;
	}

	if ((ts->tid >= DP_MAX_TIDS) ||
	    (peer->bss_peer && ts->tid == DP_NON_QOS_TID)) {
		dp_tx_capture_err("%pK: peer_id %d, tid %d > NON_QOS_TID!",
				  soc, ts->peer_id, ts->tid);
		return QDF_STATUS_E_FAILURE;
	}

	tx_tid = &peer->monitor_peer->tx_capture.tx_tid[ts->tid];

	if (!tx_tid) {
		dp_tx_capture_err("%pK: tid[%d] NULL !", soc, ts->tid);
		return QDF_STATUS_E_FAILURE;
	}

	if (tx_tid->max_ppdu_id != ts->ppdu_id)
		dp_drop_enq_msdu_on_thresh(pdev, peer, tx_tid,
					   &tx_tid->msdu_comp_q, ts->tsf);

	/* drop MSDUs of the same PPDU when adding the new buffer goes
	 * beyond the allowed limit */
	if (wlan_cfg_get_tx_capt_max_mem(soc->wlan_cfg_ctx)) {
		if ((mon_soc->dp_soc_tx_capt.last_dropped_id == ts->ppdu_id) ||
			(!dp_tx_capt_mem_check(pdev, qdf_nbuf_get_truesize(netbuf)))) {
			mon_soc->dp_soc_tx_capt.last_dropped_id = ts->ppdu_id;
			mon_soc->dp_soc_tx_capt.mem_limit_drops++;
			mon_soc->dp_soc_tx_capt.data_enq_drops++;
			return QDF_STATUS_E_NOMEM;
		} else {
			mon_soc->dp_soc_tx_capt.last_dropped_id = 0;
		}
	}

	if (!qdf_nbuf_push_head(netbuf, sizeof(struct msdu_completion_info))) {
		dp_tx_capture_err("%pK: No headroom", soc);
		return QDF_STATUS_E_NOMEM;
	}

	msdu_comp_info = (struct msdu_completion_info *)qdf_nbuf_data(netbuf);

	/* copy msdu_completion_info to control buffer */
	msdu_comp_info->ppdu_id = ts->ppdu_id;
	msdu_comp_info->peer_id = ts->peer_id;
	msdu_comp_info->tid = ts->tid;
	msdu_comp_info->first_msdu = ts->first_msdu;
	msdu_comp_info->last_msdu = ts->last_msdu;
	msdu_comp_info->msdu_part_of_amsdu = ts->msdu_part_of_amsdu;
	msdu_comp_info->transmit_cnt = ts->transmit_cnt;
	msdu_comp_info->tsf = ts->tsf;
	msdu_comp_info->status = ts->status;

	/* lock here */
	qdf_spin_lock_bh(&tx_tid->tasklet_tid_lock);
	/* add nbuf to tail queue per peer tid */
	qdf_nbuf_queue_add(&tx_tid->msdu_comp_q, netbuf);
	if (wlan_cfg_get_tx_capt_max_mem(soc->wlan_cfg_ctx)) {
		qdf_atomic_add(qdf_nbuf_get_truesize(netbuf),
			       &tx_tid->msdu_comp_bytes);
	}
	dp_tx_cap_stats_msdu_update(peer, PEER_MSDU_ENQ, 1);
	/* store last enqueue msec */
	tx_tid->last_enq_ms = qdf_system_ticks_to_msecs(qdf_system_ticks());
	/* unlock here */
	qdf_spin_unlock_bh(&tx_tid->tasklet_tid_lock);

	/* update max ppdu_id */
	tx_tid->max_ppdu_id = ts->ppdu_id;
	mon_pdev->tx_capture.last_msdu_id = ts->ppdu_id;
	mon_pdev->tx_capture.last_peer_id = ts->peer_id;

	dp_tx_capture_info("%pK: msdu_completion: ppdu_id[%d] peer_id[%d] tid[%d] rel_src[%d] status[%d] tsf[%u] A[%d] CNT[%d]",
			   soc, ts->ppdu_id, ts->peer_id, ts->tid,
			   ts->release_src,
			   ts->status, ts->tsf, ts->msdu_part_of_amsdu,
			   ts->transmit_cnt);

	return QDF_STATUS_SUCCESS;
}

/**
 * dp_tx_add_to_comp_queue_1_0() - add completion msdu to queue
 * @soc: DP Soc handle
 * @tx_desc: software Tx descriptor
 * @ts: Tx completion status from HAL/HTT descriptor
 * @peer_id: DP peer id
 *
 * Return: none
 */
QDF_STATUS dp_tx_add_to_comp_queue_1_0(struct dp_soc *soc,
				       struct dp_tx_desc_s *desc,
				       struct hal_tx_completion_status *ts,
				       uint16_t peer_id)
{
	int ret = QDF_STATUS_E_FAILURE;
	struct dp_pdev *pdev = desc->pdev;
	struct dp_peer *peer;

	peer = dp_peer_get_ref_by_id(soc, peer_id, DP_MOD_ID_TX_CAPTURE);
	if (peer &&
	    dp_peer_or_pdev_tx_cap_enabled(pdev, peer, peer->mac_addr.raw) &&
	    ((ts->status == HAL_TX_TQM_RR_FRAME_ACKED) ||
	    (ts->status == HAL_TX_TQM_RR_REM_CMD_TX) ||
	    ((ts->status == HAL_TX_TQM_RR_REM_CMD_AGED) && ts->transmit_cnt))) {
		if (qdf_unlikely(desc->pkt_offset != 0) &&
		    (qdf_nbuf_pull_head(
				desc->nbuf, desc->pkt_offset) == NULL)) {
			dp_tx_capture_err("%pK: netbuf %pK offset %d",
					  soc, desc->nbuf, desc->pkt_offset);
			dp_peer_unref_delete(peer, DP_MOD_ID_TX_CAPTURE);
			return ret;
		}

		ret = dp_update_msdu_to_list(soc, pdev, peer, ts, desc->nbuf);
	}

	if (peer)
		dp_peer_unref_delete(peer, DP_MOD_ID_TX_CAPTURE);

	return ret;
}

/**
 * get_number_of_1s(): Function to get number of 1s
 * @value: value to find
 *
 * return: number of 1s
 */
static
inline uint32_t get_number_of_1s(uint32_t value)
{
	uint32_t shift[] = {1, 2, 4, 8, 16};
	uint32_t magic_number[] = { 0x55555555, 0x33333333, 0x0F0F0F0F,
				    0x00FF00FF, 0x0000FFFF};
	uint8_t k = 0;

	for (; k <= 4; k++) {
		value = (value & magic_number[k]) +
			((value >> shift[k]) & magic_number[k]);
	}

	return value;
}

/**
 * dp_process_ppdu_stats_update_failed_bitmap(): update failed bitmap
 * @pdev: dp_pdev
 * @data: tx completion ppdu desc
 * @ppdu_id: ppdu id
 * @size: size of bitmap
 *
 * return: status
 */
void dp_process_ppdu_stats_update_failed_bitmap(struct dp_pdev *pdev,
						void *data,
						uint32_t ppdu_id,
						uint32_t size)
{
	struct cdp_tx_completion_ppdu_user *user;
	uint32_t mpdu_tried;
	uint32_t ba_seq_no;
	uint32_t start_seq;
	uint32_t num_mpdu;
	uint32_t diff;
	uint32_t carry = 0;
	uint32_t bitmask = 0;

	uint32_t i;
	uint32_t k;
	uint32_t ba_bitmap = 0;
	int last_set_bit;
	uint32_t last_ba_set_bit = 0;
	uint8_t extra_ba_mpdus = 0;
	uint32_t last_ba_seq = 0;
	uint32_t enq_ba_bitmap[CDP_BA_256_BIT_MAP_SIZE_DWORDS] = {0};
	uint32_t mpdu_enq = 0;

	user = (struct cdp_tx_completion_ppdu_user *)data;

	/* get number of mpdu from ppdu_desc */
	mpdu_tried = user->mpdu_tried_mcast + user->mpdu_tried_ucast;

	ba_seq_no = user->ba_seq_no;
	start_seq = user->start_seq;
	num_mpdu = user->num_mpdu;

	/* assumption: number of mpdu will be less than 32 */

	 dp_tx_capture_info("%pK: ppdu_id[%d] ba_seq_no[%d] start_seq_no[%d] mpdu_tried[%d]",
			    pdev->soc, ppdu_id,
			    ba_seq_no, start_seq, mpdu_tried);

	for (i = 0; i < size; i++) {
		dp_tx_capture_info("%pK: ppdu_id[%d] ba_bitmap[%x] enqueue_bitmap[%x]",
				   pdev->soc, ppdu_id,
				   user->ba_bitmap[i],
				   user->enq_bitmap[i]);
	}

	/* Handle sequence no. wraparound */
	if (start_seq <= ba_seq_no) {
		diff = ba_seq_no - start_seq;
		/* Sequence delta of more than 2048 is considered wraparound
		 * and we extend start_seq to be more than ba_seq just to
		 * adjust failed_bitmap
		 */
		if (qdf_unlikely(diff > (IEEE80211_SEQ_MAX / 2))) {
			diff = (start_seq - ba_seq_no) &
				(IEEE80211_SEQ_MAX - 1);
			start_seq = ba_seq_no + diff;
		}
	} else {
		diff = start_seq - ba_seq_no;
		/* Sequence delta of more than 2048 is considered wraparound
		 * and we extend ba_seq to be more than start_seq just to
		 * adjust failed_bitmap
		 */
		if (qdf_unlikely(diff > (IEEE80211_SEQ_MAX / 2))) {
			diff = (ba_seq_no - start_seq) &
				(IEEE80211_SEQ_MAX - 1);
			ba_seq_no = start_seq + diff;
		}
	}

	if (num_mpdu > mpdu_tried)
		extra_ba_mpdus = 1;

	/* Adjust failed_bitmap to start from same seq_no as enq_bitmap */
	last_set_bit = 0;
	if (start_seq <= ba_seq_no) {
		bitmask = (1 << diff) - 1;
		for (i = 0; i < size; i++) {
			ba_bitmap = user->ba_bitmap[i];

			user->failed_bitmap[i] = (ba_bitmap << diff);
			user->failed_bitmap[i] |= (bitmask & carry);

			carry = ((ba_bitmap & (bitmask << (32 - diff))) >>
				(32 - diff));

			if (user->failed_bitmap[i]) {
				last_ba_set_bit = i * 32 +
					  qdf_fls(user->failed_bitmap[i]) - 1;
				if (extra_ba_mpdus) {
					enq_ba_bitmap[i] =
						user->failed_bitmap[i];
					dp_tx_capture_info("%pK: i=%d failed_bitmap[%d] = 0x%x last_ba_set_bit:%d\n",
							   pdev->soc, i, i,
							   user->failed_bitmap[i],
							   last_ba_set_bit);
				}
			}

			if (extra_ba_mpdus) {
				user->enq_bitmap[i] = enq_ba_bitmap[i];
			} else {
				user->failed_bitmap[i] = user->enq_bitmap[i] &
							 user->failed_bitmap[i];
			}

			if (user->enq_bitmap[i]) {
				last_set_bit = i * 32 +
					qdf_fls(user->enq_bitmap[i]) - 1;
			}
		}
	} else {
		/* array index */
		k = diff >> 5;
		diff = diff & 0x1F;

		bitmask = (1 << diff) - 1;
		for (i = 0; i < size && k < size; i++, k++) {
			ba_bitmap = user->ba_bitmap[k];
			user->failed_bitmap[i] = ba_bitmap >> diff;
			/* get next ba_bitmap */
			if (k < (size - 1))
				ba_bitmap = user->ba_bitmap[k + 1];
			else
				ba_bitmap = 0;

			carry = (ba_bitmap & bitmask);
			user->failed_bitmap[i] |=
				((carry & bitmask) << (32 - diff));

			if (user->failed_bitmap[i]) {
				last_ba_set_bit = i * 32 +
					  qdf_fls(user->failed_bitmap[i]) - 1;
				if (extra_ba_mpdus) {
					enq_ba_bitmap[i] =
						user->failed_bitmap[i];
					dp_tx_capture_info("%pK: i=%d failed_bitmap[%d] = 0x%x last_ba_set_bit:%d\n",
							   pdev->soc, i, i,
							   user->failed_bitmap[i],
							   last_ba_set_bit);
				}
			}

			if (extra_ba_mpdus) {
				user->enq_bitmap[i] = enq_ba_bitmap[i];
			} else {
				user->failed_bitmap[i] = user->enq_bitmap[i] &
							 user->failed_bitmap[i];
			}

			if (user->enq_bitmap[i]) {
				last_set_bit = i * 32 +
					qdf_fls(user->enq_bitmap[i]) - 1;
			}
		}
	}
	user->last_enq_seq = user->start_seq + last_set_bit;
	user->ba_size = user->last_enq_seq - user->start_seq + 1;

	last_ba_seq = user->start_seq + last_ba_set_bit;

	/* mpdu_tried should be always higher than last ba bit in ba bitmap */
	if ((user->mpdu_tried_ucast) &&
	    (user->mpdu_tried_ucast < (last_set_bit + 1))) {
		for (i = 0; i < size; i++)
			mpdu_enq += get_number_of_1s(user->enq_bitmap[i]);

		if (user->mpdu_tried_ucast < mpdu_enq) {
			for (i = 0; i < size; i++)
				dp_tx_capture_info("%pK: ppdu_id[%d] ba_bitmap[%x] enqueue_bitmap[%x] failed_bitmap[%x]",
						   pdev->soc, ppdu_id,
						   user->ba_bitmap[i],
						   user->enq_bitmap[i],
						   user->failed_bitmap[i]);

				dp_tx_capture_info("%pK: last_set_bit:%d mpdu_tried_ucast %d mpdu_enq %d\n",
						   pdev->soc, last_set_bit,
						   user->mpdu_tried_ucast,
						   mpdu_enq);

			user->mpdu_tried_ucast = mpdu_enq;
		}
	}

	if (extra_ba_mpdus) {
		dp_tx_capture_info("%pK: ppdu_id:%u ba_size:%u modified_ba_size:%u last_ba_set_bit:%u start_seq: %u\n",
				   pdev->soc,
				   ppdu_id,
				   user->ba_size,
				   last_ba_seq - user->start_seq + 1,
				   last_ba_set_bit, user->start_seq);
		user->ba_size = last_ba_seq - user->start_seq + 1;

		user->last_enq_seq = last_ba_seq;
	}
}

/*
 * dp_soc_set_txrx_ring_map_single()
 * @dp_soc: DP handler for soc
 *
 * Return: Void
 */
static void dp_soc_set_txrx_ring_map_single(struct dp_soc *soc)
{
	uint32_t i;

	for (i = 0; i < WLAN_CFG_INT_NUM_CONTEXTS; i++) {
		soc->tx_ring_map[i] =
			dp_cpu_ring_map[DP_SINGLE_TX_RING_MAP][i];
	}
}

static void  dp_peer_free_msdu_q(struct dp_soc *soc,
				 struct dp_peer *peer,
				 void *arg)
{
	if (!peer->monitor_peer)
		return;

	/* disable tx capture flag in peer */
	peer->monitor_peer->tx_cap_enabled = 0;
	dp_peer_tid_queue_cleanup(peer);
}

static void  dp_peer_init_msdu_q(struct dp_soc *soc,
				 struct dp_peer *peer,
				 void *arg)
{
	if (!peer->monitor_peer)
		return;

	dp_peer_tid_queue_init(peer);
}

/*
 * dp_soc_is_tx_capture_set_in_pdev() - API to get tx capture set in any pdev
 * @soc_handle: DP_SOC handle
 *
 * return: true
 */
uint8_t
dp_soc_is_tx_capture_set_in_pdev(struct dp_soc *soc)
{
	struct dp_pdev *pdev;
	uint8_t pdev_tx_capture = 0;
	uint8_t i;
	struct dp_mon_pdev *mon_pdev;

	for (i = 0; i < MAX_PDEV_CNT; i++) {
		pdev = soc->pdev_list[i];
		if (!pdev || !pdev->monitor_pdev)
			continue;

		mon_pdev = pdev->monitor_pdev;
		if (!mon_pdev->tx_capture_enabled)
			continue;

		pdev_tx_capture++;
	}

	return pdev_tx_capture;
}

/*
 * dp_enh_tx_capture_disable()- API to disable enhanced tx capture
 * @pdev_handle: DP_PDEV handle
 * Return: void
 */
void
dp_enh_tx_capture_disable(struct dp_pdev *pdev)
{
	int i, j;
	struct dp_mon_pdev *mon_pdev = pdev->monitor_pdev;
	bool mem_limit_flag =
		wlan_cfg_get_tx_capt_max_mem(pdev->soc->wlan_cfg_ctx);

	dp_peer_tx_cap_del_all_filter(pdev);
	mon_pdev->tx_capture_enabled = CDP_TX_ENH_CAPTURE_DISABLED;

	if (!dp_soc_is_tx_capture_set_in_pdev(pdev->soc))
		dp_soc_set_txrx_ring_map(pdev->soc);

	dp_h2t_cfg_stats_msg_send(pdev,
				  DP_PPDU_STATS_CFG_ENH_STATS,
				  pdev->pdev_id);

	dp_pdev_iterate_peer(pdev, dp_peer_free_msdu_q, NULL,
			     DP_MOD_ID_TX_CAPTURE);

	for (i = 0; i < TXCAP_MAX_TYPE; i++) {
		for (j = 0; j < TXCAP_MAX_SUBTYPE; j++) {
			qdf_nbuf_queue_t *retries_q;
			uint32_t nbytes;
			struct dp_mon_soc *mon_soc = pdev->soc->monitor_soc;

			if (mem_limit_flag) {
				 nbytes =
				 get_queue_bytes(&mon_pdev->tx_capture.ctl_mgmt_q[i][j]);
				 qdf_atomic_sub(nbytes,
					       &mon_soc->dp_soc_tx_capt.ppdu_mgmt_bytes);
			}

			qdf_spin_lock_bh(
				&mon_pdev->tx_capture.ctl_mgmt_lock[i][j]);
			TX_CAP_NBUF_QUEUE_FREE(
				&mon_pdev->tx_capture.ctl_mgmt_q[i][j]);
			qdf_spin_unlock_bh(
				&mon_pdev->tx_capture.ctl_mgmt_lock[i][j]);
			retries_q =
				&mon_pdev->tx_capture.retries_ctl_mgmt_q[i][j];
			if (!qdf_nbuf_is_queue_empty(retries_q)) {
				if (mem_limit_flag) {
					nbytes = get_queue_bytes(retries_q);
					qdf_atomic_sub(nbytes, &mon_soc->dp_soc_tx_capt.ppdu_mgmt_bytes);
				}
				TX_CAP_NBUF_QUEUE_FREE(retries_q);
			}
		}
	}

	dp_tx_capture_info("%pK: Mode change request done cur mode - %d user_mode - %d\n",
			   pdev->soc, mon_pdev->tx_capture_enabled,
			   CDP_TX_ENH_CAPTURE_DISABLED);
}

/*
 * dp_enh_tx_capture_enable()- API to disable enhanced tx capture
 * @pdev_handle: DP_PDEV handle
 * @user_mode: user mode
 *
 * Return: void
 */
void
dp_enh_tx_capture_enable(struct dp_pdev *pdev, uint8_t user_mode)
{
	struct dp_mon_pdev *mon_pdev = pdev->monitor_pdev;

	dp_pdev_iterate_peer(pdev, dp_peer_init_msdu_q, NULL,
			     DP_MOD_ID_TX_CAPTURE);

	if (!dp_soc_is_tx_capture_set_in_pdev(pdev->soc))
		dp_soc_set_txrx_ring_map_single(pdev->soc);

	if (!mon_pdev->pktlog_ppdu_stats)
		dp_h2t_cfg_stats_msg_send(pdev,
					  DP_PPDU_STATS_CFG_SNIFFER,
					  pdev->pdev_id);

	mon_pdev->tx_capture.msdu_threshold_drop = 0;
	mon_pdev->tx_capture_enabled = user_mode;
	dp_tx_capture_info("%pK: Mode change request done cur mode - %d user_mode - %d\n",
			   pdev->soc, mon_pdev->tx_capture_enabled, user_mode);
}

/*
 * dp_enh_tx_cap_mode_change()- API to enable/disable enhanced tx capture
 * @pdev_handle: DP_PDEV handle
 * @user_mode: user provided value
 *
 * Return: void
 */
static void
dp_enh_tx_cap_mode_change(struct dp_pdev *pdev, uint8_t user_mode)
{
	if (user_mode == CDP_TX_ENH_CAPTURE_DISABLED)
		dp_enh_tx_capture_disable(pdev);
	else
		dp_enh_tx_capture_enable(pdev, user_mode);
}

/*
 * dp_config_enh_tx_capture_1_0()- API to enable/disable enhanced tx capture
 * @pdev_handle: DP_PDEV handle
 * @val: user provided value
 *
 * Return: QDF_STATUS
 */
QDF_STATUS
dp_config_enh_tx_capture_1_0(struct dp_pdev *pdev, uint8_t val)
{
	struct dp_mon_pdev *mon_pdev = pdev->monitor_pdev;

	qdf_atomic_set(&mon_pdev->tx_capture.tx_cap_usr_mode, val);

	dp_tx_capture_info("%pK: User mode change requested - %d\n",
			   pdev->soc,
			   qdf_atomic_read(&mon_pdev->tx_capture.tx_cap_usr_mode));

	return QDF_STATUS_SUCCESS;
}

/**
 * dp_tx_print_bitmap(): Function to print bitmap
 * @pdev: dp_pdev
 * @ppdu_desc: ppdu completion descriptor
 * @user_inder: user index
 * @ppdu_id: ppdu id
 *
 * return: status
 */
static
QDF_STATUS dp_tx_print_bitmap(struct dp_pdev *pdev,
			      struct cdp_tx_completion_ppdu *ppdu_desc,
			      uint32_t user_index,
			      uint32_t ppdu_id)
{
	struct cdp_tx_completion_ppdu_user *user;
	uint8_t i;
	uint32_t mpdu_tried;
	uint32_t ba_seq_no;
	uint32_t start_seq;
	uint32_t num_mpdu;
	uint32_t fail_num_mpdu = 0;

	user = &ppdu_desc->user[user_index];

	/* get number of mpdu from ppdu_desc */
	mpdu_tried = user->mpdu_tried_mcast + user->mpdu_tried_ucast;

	ba_seq_no = user->ba_seq_no;
	start_seq = user->start_seq;
	num_mpdu = user->mpdu_success;

	if (user->tid >= DP_MAX_TIDS) {
		dp_tx_capture_err("%pK: ppdu[%d] peer_id[%d] TID[%d] > NON_QOS_TID!",
				  pdev->soc, ppdu_id, user->peer_id, user->tid);

		return QDF_STATUS_E_FAILURE;
	}

	if (mpdu_tried != num_mpdu) {
		dp_tx_capture_info("%pK: ppdu[%d] peer[%d] tid[%d] ba[%d] start[%d] mpdu_tri[%d] num_mpdu[%d] is_mcast[%d]",
				   pdev->soc, ppdu_id, user->peer_id, user->tid,
				   ba_seq_no, start_seq, mpdu_tried,
				   num_mpdu, user->is_mcast);

		for (i = 0; i < CDP_BA_256_BIT_MAP_SIZE_DWORDS; i++) {
			dp_tx_capture_err("%pK: ppdu_id[%d] ba_bitmap[0x%x] enqueue_bitmap[0x%x] failed_bitmap[0x%x]",
					  pdev->soc, ppdu_id,
					  user->ba_bitmap[i],
					  user->enq_bitmap[i],
					  user->failed_bitmap[i]);

			fail_num_mpdu +=
				get_number_of_1s(user->failed_bitmap[i]);
		}
	}

	if (fail_num_mpdu == num_mpdu && num_mpdu)
		dp_tx_capture_debug("%pK: ppdu_id[%d] num_mpdu[%d, %d]",
				    pdev->soc, ppdu_id, num_mpdu,
				    fail_num_mpdu);

	return QDF_STATUS_SUCCESS;
}

/**
 * dp_ppdu_desc_debug_print(): Function to print ppdu_desc
 * @ppdu_desc: ppdu desc pointer
 * @usr_idx: user index
 * @func: caller function name
 * @line: caller function line number
 *
 * return: void
 */
void dp_ppdu_desc_debug_print(struct cdp_tx_completion_ppdu *ppdu_desc,
			      uint8_t usr_idx, const char *func, uint32_t line)
{
	struct cdp_tx_completion_ppdu_user *user;
	uint8_t num_users;

	num_users = ppdu_desc->num_users;

	dp_tx_capture_info("PID: %d, BPID: %d SCHED: %d usr_idx: %d TLV_BITMAP[0x%x] num_users:%d",
			   ppdu_desc->ppdu_id, ppdu_desc->bar_ppdu_id,
			   ppdu_desc->sched_cmdid,
			   usr_idx, ppdu_desc->tlv_bitmap,
			   ppdu_desc->num_users);

	user = &ppdu_desc->user[usr_idx];
	dp_tx_capture_info("P[%d] CS:%d S_SEQ: %d L_ENQ_SEQ:%d BA_SEQ:%d BA_SZ:%d M[TRI: %d, SUC: %d] ENQ[%x:%x:%x:%x] BA[%x:%x:%x:%x] F[%x:%x:%x:%x] tlv[0x%x]",
			   user->peer_id,
			   user->completion_status,
			   user->start_seq, user->last_enq_seq,
			   user->ba_seq_no, user->ba_size,
			   user->mpdu_tried_ucast + user->mpdu_tried_mcast,
			   user->mpdu_success,
			   user->enq_bitmap[0], user->enq_bitmap[1],
			   user->enq_bitmap[2], user->enq_bitmap[3],
			   user->ba_bitmap[0], user->ba_bitmap[1],
			   user->ba_bitmap[2], user->ba_bitmap[3],
			   user->failed_bitmap[0], user->failed_bitmap[1],
			   user->failed_bitmap[2], user->failed_bitmap[3],
			   user->tlv_bitmap);
}

/*
 * dp_peer_tx_wds_addr_add() – Update WDS peer to include 4th address
 * @peer: Datapath peer
 * @addr4_mac_addr: Source MAC address for WDS TX
 *
 */
static
void dp_peer_tx_wds_addr_add(struct dp_peer *peer, uint8_t *addr4_mac_addr)
{
	struct ieee80211_frame_addr4 *ptr_wh = NULL;
	struct dp_vdev *vdev = NULL;

	if (!peer) {
		qdf_err("peer is NULL!");
		return;
	}

	vdev = peer->vdev;
	if (!vdev) {
		qdf_err("vdev is NULL!");
		return;
	}

	ptr_wh = &peer->monitor_peer->tx_capture.tx_wifi_addr4_hdr;
	qdf_mem_copy(ptr_wh->i_addr1,
		     peer->mac_addr.raw,
		     QDF_MAC_ADDR_SIZE);
	qdf_mem_copy(ptr_wh->i_addr2,
		     vdev->mac_addr.raw,
		     QDF_MAC_ADDR_SIZE);
	qdf_mem_copy(ptr_wh->i_addr4,
		     addr4_mac_addr,
		     QDF_MAC_ADDR_SIZE);
}

/*
 * dp_peer_tx_update_80211_wds_hdr() – Update 80211 frame header to include a
 * 4 address frame, and set QoS related information if necessary
 * @pdev: Physical device reference
 * @peer: Datapath peer
 * @data: ppdu_descriptor
 * @nbuf: 802.11 frame
 * @ether_type: ethernet type
 * @src_addr: ether shost address
 * @usr_idx: user index
 *
 */
static uint32_t dp_tx_update_80211_wds_hdr(struct dp_pdev *pdev,
					   struct dp_peer *peer,
					   void *data,
					   qdf_nbuf_t nbuf,
					   uint16_t ether_type,
					   uint8_t *dst_addr,
					   uint8_t usr_idx,
					   bool is_amsdu)
{
	struct cdp_tx_completion_ppdu *ppdu_desc;
	struct cdp_tx_completion_ppdu_user *user;
	uint32_t mpdu_buf_len, frame_size;
	uint8_t *ptr_hdr;
	uint16_t eth_type = qdf_htons(ether_type);
	struct ieee80211_qosframe_addr4 *ptr_wh;
	struct dp_mon_peer *mon_peer = peer->monitor_peer;

	ppdu_desc = (struct cdp_tx_completion_ppdu *)data;
	user = &ppdu_desc->user[usr_idx];

	ptr_wh = &mon_peer->tx_capture.tx_wifi_addr4_qos_hdr;

	/*
	 * update framectrl only for first ppdu_id
	 * rest of mpdu will have same frame ctrl
	 * mac address and duration
	 */
	if (ppdu_desc->ppdu_id != mon_peer->tx_capture.tx_wifi_ppdu_id) {
		ptr_wh->i_fc[1] = (ppdu_desc->frame_ctrl & 0xFF00) >> 8;
		ptr_wh->i_fc[0] = (ppdu_desc->frame_ctrl & 0xFF);

		ptr_wh->i_dur[1] = (ppdu_desc->tx_duration & 0xFF00) >> 8;
		ptr_wh->i_dur[0] = (ppdu_desc->tx_duration & 0xFF);

		ptr_wh->i_qos[1] = (user->qos_ctrl & 0xFF00) >> 8;
		ptr_wh->i_qos[0] = (user->qos_ctrl & 0xFF);

		mon_peer->tx_capture.tx_wifi_ppdu_id = ppdu_desc->ppdu_id;
	}

	if (is_amsdu) {
		ptr_wh->i_qos[0] |=  IEEE80211_QOS_AMSDU;
		/* update addr3 and addr4 with BSSID in an AMSDU frame */
		qdf_mem_copy(ptr_wh->i_addr3, ptr_wh->i_addr2,
			     QDF_MAC_ADDR_SIZE);
		qdf_mem_copy(ptr_wh->i_addr4, ptr_wh->i_addr2,
			     QDF_MAC_ADDR_SIZE);
	} else {
		/* Update Addr 3 (DA) with DA derived from ether packet */
		qdf_mem_copy(ptr_wh->i_addr3, dst_addr, QDF_MAC_ADDR_SIZE);
	}

	frame_size = (user->tid != DP_NON_QOS_TID) ?
		      sizeof(struct ieee80211_qosframe_addr4) :
		      sizeof(struct ieee80211_frame_addr4);

	if (is_amsdu)
		mpdu_buf_len = frame_size;
	else
		mpdu_buf_len = frame_size + LLC_SNAP_HDR_LEN;

	nbuf->protocol = qdf_htons(ETH_P_802_2);

	/* update ieee80211_frame header */
	if (!qdf_nbuf_push_head(nbuf, mpdu_buf_len)) {
		dp_tx_capture_err("%pK: No headroom", pdev->soc);
		return QDF_STATUS_E_NOMEM;
	}

	ptr_hdr = (void *)qdf_nbuf_data(nbuf);
	qdf_mem_copy(ptr_hdr, ptr_wh, frame_size);

	ptr_hdr = ptr_hdr + frame_size;

	/* update LLC */
	if (!is_amsdu) {
		*ptr_hdr =  LLC_SNAP_LSAP;
		*(ptr_hdr + 1) = LLC_SNAP_LSAP;
		*(ptr_hdr + 2) = LLC_UI;
		*(ptr_hdr + 3) = 0x00;
		*(ptr_hdr + 4) = 0x00;
		*(ptr_hdr + 5) = 0x00;
		*(ptr_hdr + 6) = (eth_type & 0xFF00) >> 8;
		*(ptr_hdr + 7) = (eth_type & 0xFF);
	}
	qdf_nbuf_trim_tail(nbuf, qdf_nbuf_len(nbuf) - mpdu_buf_len);
	return 0;
}

/**
 * dp_tx_update_80211_hdr() – Update 80211 frame header to set QoS
 * related information if necessary
 * @pdev: Physical device reference
 * @peer: Datapath peer
 * @data: ppdu_descriptor
 * @nbuf: 802.11 frame
 * @ether_type: ethernet type
 * @src_addr: ether shost address
 *
 */
static uint32_t dp_tx_update_80211_hdr(struct dp_pdev *pdev,
				       struct dp_peer *peer,
				       void *data,
				       qdf_nbuf_t nbuf,
				       uint16_t ether_type,
				       uint8_t *src_addr,
				       uint8_t usr_idx,
				       bool is_amsdu)
{
	struct cdp_tx_completion_ppdu *ppdu_desc;
	struct cdp_tx_completion_ppdu_user *user;
	uint32_t mpdu_buf_len, frame_size;
	uint8_t *ptr_hdr;
	uint16_t eth_type = qdf_htons(ether_type);

	struct ieee80211_qosframe *ptr_wh;
	struct dp_mon_peer *mon_peer = peer->monitor_peer;

	ppdu_desc = (struct cdp_tx_completion_ppdu *)data;
	user = &ppdu_desc->user[usr_idx];

	ptr_wh = &mon_peer->tx_capture.tx_wifi_qos_hdr;

	/*
	 * update framectrl only for first ppdu_id
	 * rest of mpdu will have same frame ctrl
	 * mac address and duration
	 */
	if (ppdu_desc->ppdu_id != mon_peer->tx_capture.tx_wifi_ppdu_id) {
		ptr_wh->i_fc[1] = (user->frame_ctrl & 0xFF00) >> 8;
		ptr_wh->i_fc[0] = (user->frame_ctrl & 0xFF);

		ptr_wh->i_dur[1] = (ppdu_desc->tx_duration & 0xFF00) >> 8;
		ptr_wh->i_dur[0] = (ppdu_desc->tx_duration & 0xFF);

		ptr_wh->i_qos[1] = (user->qos_ctrl & 0xFF00) >> 8;
		ptr_wh->i_qos[0] = (user->qos_ctrl & 0xFF);

		mon_peer->tx_capture.tx_wifi_ppdu_id = ppdu_desc->ppdu_id;
	}

	if (is_amsdu) {
		ptr_wh->i_qos[0] |=  IEEE80211_QOS_AMSDU;
		/* update addr3 with BSSID in an AMSDU frame */
		qdf_mem_copy(ptr_wh->i_addr3, ptr_wh->i_addr2,
			     QDF_MAC_ADDR_SIZE);
	} else {
		/* Update Addr 3 (SA) with SA derived from ether packet */
		qdf_mem_copy(ptr_wh->i_addr3, src_addr, QDF_MAC_ADDR_SIZE);
	}

	frame_size = (user->tid != DP_NON_QOS_TID) ?
		      sizeof(struct ieee80211_qosframe) :
		      sizeof(struct ieee80211_frame);

	if (is_amsdu)
		mpdu_buf_len = frame_size;
	else
		mpdu_buf_len = frame_size + LLC_SNAP_HDR_LEN;

	nbuf->protocol = qdf_htons(ETH_P_802_2);

	/* update ieee80211_frame header */
	if (!qdf_nbuf_push_head(nbuf, mpdu_buf_len)) {
		dp_tx_capture_err("%pK: No headroom", pdev->soc);
		return QDF_STATUS_E_NOMEM;
	}

	ptr_hdr = (void *)qdf_nbuf_data(nbuf);
	qdf_mem_copy(ptr_hdr, ptr_wh, frame_size);

	if (!is_amsdu) {
		ptr_hdr = ptr_hdr + frame_size;

		/* update LLC */
		*ptr_hdr =  LLC_SNAP_LSAP;
		*(ptr_hdr + 1) = LLC_SNAP_LSAP;
		*(ptr_hdr + 2) = LLC_UI;
		*(ptr_hdr + 3) = 0x00;
		*(ptr_hdr + 4) = 0x00;
		*(ptr_hdr + 5) = 0x00;
		*(ptr_hdr + 6) = (eth_type & 0xFF00) >> 8;
		*(ptr_hdr + 7) = (eth_type & 0xFF);
	}

	qdf_nbuf_trim_tail(nbuf, qdf_nbuf_len(nbuf) - mpdu_buf_len);
	return 0;
}

static QDF_STATUS dp_tx_add_amsdu_llc_hdr(qdf_nbuf_t nbuf, bool is_last_msdu)
{
	qdf_ether_header_t *eh = NULL;
	struct llc_snap_hdr_t *llchdr = NULL;
	uint8_t *p_len = NULL;
	uint16_t eth_type;
	uint16_t nbuf_len = qdf_nbuf_len(nbuf);

	/*
	 * Steps-
	 * 1.nbuf->data is pointing to beginning of ethernet header.
	 * 2.push is done to create space for LLC header
	 * 3.Move 14 bytes (size of ether_header_t) to nbuf->data
	 * 4.update AMSDU subframe length
	 * 5.populate LLC and add padding if needed
	 *
	 * AMSDU format: DA | SA | LEN | LLC_SNAP | TYPE | PAYLOAD
	 */

	eh = (qdf_ether_header_t *)qdf_nbuf_data(nbuf);
	eth_type = qdf_htons(eh->ether_type);

	/* increase data area to include LLC header */
	if (qdf_unlikely(qdf_nbuf_headroom(nbuf) < LLC_SNAP_HDR_LEN)) {
		dp_tx_capture_alert("No Head room to push %zu bytes, avail:%d\n",
				    LLC_SNAP_HDR_LEN,
				    qdf_nbuf_headroom(nbuf));
		return QDF_STATUS_E_NOMEM;
	}

	qdf_nbuf_push_head(nbuf, LLC_SNAP_HDR_LEN);

	/* AMSDU subframe header */
	qdf_mem_move(qdf_nbuf_data(nbuf), eh, sizeof(qdf_ether_header_t));
	p_len = qdf_nbuf_data(nbuf) + (2 * QDF_NET_ETH_LEN);

	/*
	 * update AMSDU length field with size up to LLC not including the
	 * subframe header
	 */
	nbuf_len -= sizeof(qdf_ether_header_t);
	nbuf_len += LLC_SNAP_HDR_LEN;
	p_len[0] = (nbuf_len >> 8);
	p_len[1] = (nbuf_len & 0xFF);

	/* LLC header */
	llchdr = (struct llc_snap_hdr_t *)(qdf_nbuf_data(nbuf) +
					   sizeof(qdf_ether_header_t));
	llchdr->dsap = LLC_SNAP_LSAP;
	llchdr->ssap = LLC_SNAP_LSAP;
	llchdr->cntl = LLC_UI;
	qdf_mem_set(llchdr->org_code, 3, 0);
	llchdr->ethertype[0] = (eth_type & 0xFF00) >> 8;
	llchdr->ethertype[1] = (eth_type & 0xFF);

	/* add padding to end of A-MSDU except the last one (Spec requirement)*/
	if (!is_last_msdu) {
		nbuf_len = qdf_nbuf_len(nbuf);
		if (nbuf_len & 0x3)
			qdf_nbuf_put_tail(nbuf, 4 - (nbuf_len & 3));
	}

	return QDF_STATUS_SUCCESS;
}

/**
 * dp_tx_mon_restitch_mpdu(): Function to restitch msdu to mpdu
 * @pdev: dp_pdev
 * @peer: dp_peer
 * @head_msdu: head msdu queue
 *
 * return: status
 */
static uint32_t
dp_tx_mon_restitch_mpdu(struct dp_pdev *pdev, struct dp_peer *peer,
			struct cdp_tx_completion_ppdu *ppdu_desc,
			qdf_nbuf_queue_t *head_msdu,
			qdf_nbuf_queue_t *mpdu_q, uint8_t usr_idx)
{
	qdf_nbuf_t curr_nbuf = NULL;
	qdf_nbuf_t first_nbuf = NULL;
	qdf_nbuf_t prev_nbuf = NULL;
	qdf_nbuf_t mpdu_nbuf = NULL;
	struct msdu_completion_info *ptr_msdu_info = NULL;
	uint8_t first_msdu = 0;
	uint8_t last_msdu = 0;
	uint32_t frag_list_sum_len = 0;
	uint8_t first_msdu_not_seen = 1;
	uint16_t ether_type = 0;
	qdf_ether_header_t *eh = NULL;
	size_t msdu_comp_info_sz;
	size_t ether_hdr_sz;
	bool is_amsdu = 0;
	uint32_t msdu_bytes = 0;

	if (qdf_nbuf_is_queue_empty(head_msdu))
		return 0;

	curr_nbuf = qdf_nbuf_queue_remove(head_msdu);

	while (curr_nbuf) {
		ptr_msdu_info =
			(struct msdu_completion_info *)qdf_nbuf_data(curr_nbuf);

		first_msdu = ptr_msdu_info->first_msdu;
		last_msdu = ptr_msdu_info->last_msdu;
		is_amsdu = ptr_msdu_info->msdu_part_of_amsdu;

		eh = (qdf_ether_header_t *)(curr_nbuf->data +
					   sizeof(struct msdu_completion_info));
		ether_type = eh->ether_type;

		msdu_comp_info_sz = sizeof(struct msdu_completion_info);
		/* pull msdu_completion_info added in pre header */
		if (NULL == qdf_nbuf_pull_head(curr_nbuf, msdu_comp_info_sz)) {
			dp_tx_capture_alert(" No Head space to pull !!\n");
			qdf_assert_always(0);
		}

		if ((qdf_likely((peer->vdev->tx_encap_type !=
				 htt_cmn_pkt_type_raw))) &&
			(((ppdu_desc->frame_ctrl >> IEEE80211_FC1_SHIFT) &
			  IEEE80211_FC1_DIR_MASK) ==
			 (IEEE80211_FC1_DIR_TODS | IEEE80211_FC1_DIR_FROMDS)))
			dp_peer_tx_wds_addr_add(peer, eh->ether_shost);

		if (first_msdu && first_msdu_not_seen) {
			first_nbuf = curr_nbuf;
			frag_list_sum_len = 0;
			first_msdu_not_seen = 0;

			if (!is_amsdu) {
				ether_hdr_sz = sizeof(qdf_ether_header_t);
				/* pull ethernet header from first MSDU alone */
				if (NULL == qdf_nbuf_pull_head(curr_nbuf,
							       ether_hdr_sz)) {
					dp_tx_capture_alert(" No Head space to pull !!\n");
					qdf_assert_always(0);
				}
			}
			/* update first buffer to previous buffer */
			prev_nbuf = curr_nbuf;

		} else if (first_msdu && !first_msdu_not_seen) {
			dp_tx_capture_err("!!!!! NO LAST MSDU\n");
			/*
			 * no last msdu in a mpdu
			 * handle this case
			 */
			qdf_nbuf_free(curr_nbuf);
			/*
			 * No last msdu found because WBM comes out
			 * of order, free the pkt
			 */
			goto free_ppdu_desc_mpdu_q;
		} else if (!first_msdu && first_msdu_not_seen) {
			dp_tx_capture_err("!!!!! NO FIRST MSDU\n");
			/*
			 * no first msdu in a mpdu
			 * handle this case
			 */
			qdf_nbuf_free(curr_nbuf);
			/*
			 * no first msdu found because WBM comes out
			 * of order, free the pkt
			 */
			goto free_ppdu_desc_mpdu_q;
		} else {
			/* update current buffer to previous buffer next */
			prev_nbuf->next = curr_nbuf;
			/* move the previous buffer to next buffer */
			prev_nbuf = prev_nbuf->next;
		}

		if (is_amsdu) {
			if (dp_tx_add_amsdu_llc_hdr(curr_nbuf,
						    last_msdu ? 1 : 0) !=
			    QDF_STATUS_SUCCESS)
				goto free_ppdu_desc_mpdu_q;
		}

		frag_list_sum_len += qdf_nbuf_len(curr_nbuf);

		if (wlan_cfg_get_tx_capt_max_mem(pdev->soc->wlan_cfg_ctx)) {
			msdu_bytes += qdf_nbuf_get_truesize(curr_nbuf);
		}

		if (last_msdu) {
			mpdu_nbuf = qdf_nbuf_alloc(pdev->soc->osdev,
						   MAX_MONITOR_HEADER,
						   MAX_MONITOR_HEADER,
						   4, FALSE);

			if (!mpdu_nbuf) {
				dp_tx_capture_err("MPDU head allocation failed !!!");
				goto free_ppdu_desc_mpdu_q;
			}

			if (((ppdu_desc->frame_ctrl >> IEEE80211_FC1_SHIFT) &
				 IEEE80211_FC1_DIR_MASK) ==
			     (IEEE80211_FC1_DIR_TODS |
			      IEEE80211_FC1_DIR_FROMDS)) {
				dp_tx_update_80211_wds_hdr(pdev, peer,
							   ppdu_desc, mpdu_nbuf,
							   ether_type,
							   eh->ether_dhost,
							   usr_idx,
							   is_amsdu);
			} else {
				dp_tx_update_80211_hdr(pdev, peer,
						       ppdu_desc, mpdu_nbuf,
						       ether_type,
						       eh->ether_shost,
						       usr_idx,
						       is_amsdu);
			}

			if (wlan_cfg_get_tx_capt_max_mem(pdev->soc->wlan_cfg_ctx)) {
				struct cdp_tx_completion_ppdu_user *user;

				user = &ppdu_desc->user[usr_idx];
				user->mpdu_bytes +=
					(qdf_nbuf_get_truesize(mpdu_nbuf) + msdu_bytes);
				msdu_bytes = 0;
			}

			/*
			 * first nbuf will hold list of msdu
			 * stored in prev_nbuf
			 */
			qdf_nbuf_append_ext_list(mpdu_nbuf,
						 first_nbuf,
						 frag_list_sum_len);

			/* add mpdu to mpdu queue */
			qdf_nbuf_queue_add(mpdu_q, mpdu_nbuf);
			first_nbuf = NULL;
			mpdu_nbuf = NULL;

			/* next msdu will start with first msdu */
			first_msdu_not_seen = 1;
			goto check_for_next_msdu;
		}

		/* get next msdu from the head_msdu */
		curr_nbuf = qdf_nbuf_queue_remove(head_msdu);

		if (!curr_nbuf) {
			/* msdu missed in list */
			dp_tx_capture_err("!!!! WAITING for msdu but list empty !!!!");

			/* for incomplete list, free up the queue */
			goto free_ppdu_desc_mpdu_q;
		}

		continue;

check_for_next_msdu:
		if (qdf_nbuf_is_queue_empty(head_msdu))
			return 0;
		curr_nbuf = qdf_nbuf_queue_remove(head_msdu);
	}

	return 0;

free_ppdu_desc_mpdu_q:
	/* free already chained msdu pkt */
	while (first_nbuf) {
		curr_nbuf = first_nbuf;
		first_nbuf = first_nbuf->next;
		qdf_nbuf_free(curr_nbuf);
	}

	/* free allocated mpdu hdr */
	if (mpdu_nbuf)
		qdf_nbuf_free(mpdu_nbuf);
	/* free queued remaining msdu pkt per ppdu */
	TX_CAP_NBUF_QUEUE_FREE(head_msdu);
	/* free queued mpdu per ppdu */
	TX_CAP_NBUF_QUEUE_FREE(mpdu_q);

	return 0;
}

/**
 * dp_tx_msdu_dequeue(): Function to dequeue msdu from peer based tid
 * @peer: dp_peer
 * @ppdu_id: ppdu_id
 * @tid: tid
 * @num_msdu: number of msdu
 * @head: head queue
 * @start_tsf: start tsf from ppdu_desc
 * @end_tsf: end tsf from ppdu_desc
 *
 * return: status
 */
static
uint32_t dp_tx_msdu_dequeue(struct dp_peer *peer, uint32_t ppdu_id,
			    uint16_t tid, uint32_t num_msdu,
			    qdf_nbuf_queue_t *head,
			    qdf_nbuf_queue_t *head_xretries,
			    uint32_t start_tsf, uint32_t end_tsf)
{
	struct dp_tx_tid *tx_tid  = NULL;
	uint32_t msdu_ppdu_id;
	qdf_nbuf_t curr_msdu = NULL;
	struct msdu_completion_info *ptr_msdu_info = NULL;
	uint32_t wbm_tsf = 0xffff;
	uint32_t matched = 0;
	qdf_nbuf_queue_t temp_defer_q;
	struct dp_soc *soc = NULL;
	struct dp_mon_soc *mon_soc = NULL;

	if (qdf_unlikely(!peer))
		return 0;

	/* Non-QOS frames are being indicated with TID 0
	 * in WBM completion path, an hence we should
	 * TID 0 to reap MSDUs from completion path
	 */
	if (qdf_unlikely(tid == DP_NON_QOS_TID))
		tid = 0;

	tx_tid = &peer->monitor_peer->tx_capture.tx_tid[tid];

	if (qdf_unlikely(!tx_tid))
		return 0;

	soc = peer->vdev->pdev->soc;
	mon_soc = soc->monitor_soc;

	/* Initialize temp list */
	qdf_nbuf_queue_init(&temp_defer_q);

	/* lock here */
	qdf_spin_lock_bh(&tx_tid->tasklet_tid_lock);
	qdf_nbuf_queue_append(&temp_defer_q, &tx_tid->msdu_comp_q);

	if (wlan_cfg_get_tx_capt_max_mem(soc->wlan_cfg_ctx)) {
		qdf_atomic_add(qdf_atomic_read(&tx_tid->msdu_comp_bytes),
			       &mon_soc->dp_soc_tx_capt.ppdu_bytes);
		qdf_atomic_set(&tx_tid->msdu_comp_bytes, 0);
	}

	qdf_nbuf_queue_init(&tx_tid->msdu_comp_q);
	/* unlock here */
	qdf_spin_unlock_bh(&tx_tid->tasklet_tid_lock);

	/* lock here */
	qdf_spin_lock_bh(&tx_tid->tid_lock);

	qdf_nbuf_queue_append(&tx_tid->defer_msdu_q, &temp_defer_q);

	if (qdf_nbuf_is_queue_empty(&tx_tid->defer_msdu_q)) {
		/* release lock here */
		qdf_spin_unlock_bh(&tx_tid->tid_lock);
		return 0;
	}

	curr_msdu = qdf_nbuf_queue_first(&tx_tid->defer_msdu_q);

	while (curr_msdu) {
		if (qdf_nbuf_queue_len(head) == num_msdu) {
			matched = 1;
			break;
		}

		ptr_msdu_info =
			(struct msdu_completion_info *)qdf_nbuf_data(curr_msdu);

		msdu_ppdu_id = ptr_msdu_info->ppdu_id;

		wbm_tsf = ptr_msdu_info->tsf;

		if ((ptr_msdu_info->status == HAL_TX_TQM_RR_REM_CMD_TX) ||
		    (ptr_msdu_info->status == HAL_TX_TQM_RR_REM_CMD_AGED)) {
			/* Frames removed due to excessive retries */
			qdf_nbuf_queue_remove(&tx_tid->defer_msdu_q);
			if (wlan_cfg_get_tx_capt_max_mem(soc->wlan_cfg_ctx)) {
				qdf_atomic_sub(qdf_nbuf_get_truesize(curr_msdu),
					       &mon_soc->dp_soc_tx_capt.ppdu_bytes);
			}
			qdf_nbuf_queue_add(head_xretries, curr_msdu);
			dp_tx_cap_stats_msdu_update(peer, PEER_MSDU_XRETRY, 1);
			curr_msdu = qdf_nbuf_queue_first(
				&tx_tid->defer_msdu_q);
			continue;
		}

		if (wbm_tsf > end_tsf) {
			/* PPDU being matched is older than MSDU at head of
			 * completion queue. Return matched=1 to skip PPDU
			 */
			matched = 1;
			break;
		}

		if (wbm_tsf && (wbm_tsf < start_tsf)) {
			/* remove the aged packet */
			qdf_nbuf_queue_remove(&tx_tid->defer_msdu_q);
			if (wlan_cfg_get_tx_capt_max_mem(soc->wlan_cfg_ctx)) {
				qdf_atomic_sub(qdf_nbuf_get_truesize(curr_msdu),
					       &mon_soc->dp_soc_tx_capt.ppdu_bytes);
			}

			qdf_nbuf_free(curr_msdu);
			dp_tx_cap_stats_msdu_update(peer, PEER_MSDU_DROP, 1);
			curr_msdu = qdf_nbuf_queue_first(
					&tx_tid->defer_msdu_q);
			continue;
		}

		if (msdu_ppdu_id == ppdu_id) {
			/* remove head */
			qdf_nbuf_queue_remove(&tx_tid->defer_msdu_q);
			if (wlan_cfg_get_tx_capt_max_mem(soc->wlan_cfg_ctx)) {
				qdf_atomic_sub(qdf_nbuf_get_truesize(curr_msdu),
					       &mon_soc->dp_soc_tx_capt.ppdu_bytes);
			}
			/* add msdu to head queue */
			qdf_nbuf_queue_add(head, curr_msdu);
			dp_tx_cap_stats_msdu_update(peer, PEER_MSDU_DEQ, 1);
			/* get next msdu from defer_msdu_q */
			curr_msdu = qdf_nbuf_queue_first(&tx_tid->defer_msdu_q);
			continue;
		} else {
			/*
			 * at this point wbm_tsf is inbetween start_tsf and
			 * end tsf but there is a mismatch in ppdu_id
			 */
			break;
		}
	}

	/* store last dequeue time in msec */
	tx_tid->last_deq_ms = qdf_system_ticks_to_msecs(qdf_system_ticks());
	/* release lock here */
	qdf_spin_unlock_bh(&tx_tid->tid_lock);

	return matched;
}

/**
 * dp_tx_cap_nbuf_list_get_ref() - get nbuf_list reference
 * @ptr_nbuf_list: dp_tx_cap_nbuf_list list
 *
 * Return: reference count
 */
static inline uint8_t
dp_tx_cap_nbuf_list_get_ref(struct dp_tx_cap_nbuf_list *ptr_nbuf_list)
{
	return ptr_nbuf_list->ref_cnt;
}

/**
 * dp_tx_cap_nbuf_list_dec_ref() - dec nbuf_list reference
 * @ptr_nbuf_list: dp_tx_cap_nbuf_list list
 *
 * Return: none
 */
static inline
void dp_tx_cap_nbuf_list_dec_ref(struct dp_tx_cap_nbuf_list *ptr_nbuf_list)
{
	ptr_nbuf_list->ref_cnt--;
	if (!ptr_nbuf_list->ref_cnt)
		ptr_nbuf_list->nbuf_ppdu = NULL;
}

/**
 * dp_tx_cap_nbuf_list_inc_ref() - inc nbuf_list reference
 * @ptr_nbuf_list: dp_tx_cap_nbuf_list list
 *
 * Return: none
 */
static inline
void dp_tx_cap_nbuf_list_inc_ref(struct dp_tx_cap_nbuf_list *ptr_nbuf_list)
{
	ptr_nbuf_list->ref_cnt++;
}

/**
 * dp_tx_cap_nbuf_list_update_ref() - update nbuf_list reference
 * @ptr_nbuf_list: dp_tx_cap_nbuf_list list
 * @ref_cnt: reference count
 *
 * Return: none
 */
static inline void
dp_tx_cap_nbuf_list_update_ref(struct dp_tx_cap_nbuf_list *ptr_nbuf_list,
			       uint8_t ref_cnt)
{
	ptr_nbuf_list->ref_cnt = ref_cnt;
}

/**
 * get_mpdu_clone_from_next_ppdu(): Function to clone missing mpdu from
 * next ppdu
 * @nbuf_ppdu_list: nbuf list
 * @ppdu_desc_cnt: ppdu_desc_cnt
 * @missed_seq_no:
 * @ppdu_id: ppdu_id
 * @mpdu_info: cdp_tx_indication_mpdu_info
 *
 * return: void
 */
static qdf_nbuf_t
get_mpdu_clone_from_next_ppdu(struct dp_tx_cap_nbuf_list nbuf_list[],
			      uint32_t ppdu_desc_cnt,
			      uint16_t missed_seq_no,
			      uint16_t peer_id, uint32_t ppdu_id,
			      uint8_t usr_idx)
{
	struct cdp_tx_completion_ppdu *ppdu_desc = NULL;
	struct cdp_tx_completion_ppdu_user *user;
	qdf_nbuf_t mpdu = NULL;
	struct dp_tx_cap_nbuf_list *ptr_nbuf_list;
	qdf_nbuf_t nbuf_ppdu;
	uint32_t i = 0;
	uint32_t found = 0;
	uint32_t seq_no = 0;
	uint32_t mpdu_q_len;

	for (i = 1; i < ppdu_desc_cnt; i++) {
		ptr_nbuf_list = &nbuf_list[i];
		nbuf_ppdu = ptr_nbuf_list->nbuf_ppdu;
		ppdu_desc = (struct cdp_tx_completion_ppdu *)
				qdf_nbuf_data(nbuf_ppdu);
		user = &ppdu_desc->user[usr_idx];

		if (user->skip == 1)
			continue;

		/* check if seq number is between the range */
		if ((peer_id == user->peer_id) &&
		    ((missed_seq_no >= user->start_seq) &&
		    (missed_seq_no <= user->last_enq_seq))) {
			seq_no = user->start_seq;
			if (SEQ_BIT(user->failed_bitmap,
				    (missed_seq_no - seq_no))) {
				found = 1;
				break;
			}
		}
	}

	if (found == 0) {
		/* mpdu not found in sched cmd id */
		dp_tx_capture_debug("peer_id[%d] missed seq_no[%d] ppdu_id[%d] [%d] not found!!!",
				    peer_id,
				    missed_seq_no, ppdu_id, ppdu_desc_cnt);
		return NULL;
	}

	dp_tx_capture_debug("peer_id[%d] seq_no[%d] missed ppdu_id[%d] m[%d] found in ppdu_id[%d]!!",
			    peer_id,
			    missed_seq_no, ppdu_id,
			    (missed_seq_no - seq_no), ppdu_desc->ppdu_id);

	mpdu = qdf_nbuf_queue_first(&ppdu_desc->user[usr_idx].mpdu_q);
	mpdu_q_len = qdf_nbuf_queue_len(&ppdu_desc->user[usr_idx].mpdu_q);
	if (!mpdu) {
		/* bitmap shows it found  sequence number, but
		 * MPDU not found in PPDU
		 */
		dp_tx_capture_err("missed seq_no[%d] ppdu_id[%d] [%d] found but queue empty!!!",
				  missed_seq_no, ppdu_id, ppdu_desc_cnt);
		if (mpdu_q_len)
			qdf_assert_always(0);

		return NULL;
	}

	for (i = 0; i < (missed_seq_no - seq_no); i++) {
		mpdu = mpdu->next;
		if (!mpdu) {
			/*
			 * bitmap shows it found  sequence number,
			 * but queue empty, do we need to allocate
			 * skb and send instead of NULL ?
			 * add counter here:
			 */
			return NULL;
		}
	}

	if (!mpdu)
		return NULL;

	return qdf_nbuf_copy_expand_fraglist(mpdu, MAX_MONITOR_HEADER, 0);
}

/**
 * dp_tx_update_user_mpdu_info(): Function to update mpdu info
 * from ppdu_desc
 * @ppdu_id: ppdu_id
 * @mpdu_info: cdp_tx_indication_mpdu_info
 * @user: cdp_tx_completion_ppdu_user
 *
 * return: void
 */
static void
dp_tx_update_user_mpdu_info(uint32_t ppdu_id,
			    struct cdp_tx_indication_mpdu_info *mpdu_info,
			    struct cdp_tx_completion_ppdu_user *user)
{
	mpdu_info->ppdu_id = ppdu_id;

	mpdu_info->frame_ctrl = user->frame_ctrl;
	mpdu_info->qos_ctrl = user->qos_ctrl;
	mpdu_info->tid = user->tid;
	mpdu_info->ltf_size = user->ltf_size;
	mpdu_info->he_re = user->he_re;
	mpdu_info->txbf = user->txbf;
	mpdu_info->bw = user->bw;
	mpdu_info->nss = user->nss;
	mpdu_info->mcs = user->mcs;
	mpdu_info->preamble = user->preamble;
	mpdu_info->gi = user->gi;

	mpdu_info->ack_rssi = user->ack_rssi[0];
	mpdu_info->tx_rate = user->tx_rate;
	mpdu_info->ldpc = user->ldpc;
	mpdu_info->ppdu_cookie = user->ppdu_cookie;

	mpdu_info->long_retries = user->long_retries;
	mpdu_info->short_retries = user->short_retries;
	mpdu_info->completion_status = user->completion_status;

	qdf_mem_copy(mpdu_info->mac_address, user->mac_addr, 6);

	mpdu_info->ba_start_seq = user->ba_seq_no;

	qdf_mem_copy(mpdu_info->ba_bitmap, user->ba_bitmap,
		     CDP_BA_256_BIT_MAP_SIZE_DWORDS * sizeof(uint32_t));
}

static inline
void dp_tx_update_sequence_number(qdf_nbuf_t nbuf, uint32_t seq_no)
{
	struct ieee80211_frame *ptr_wh = NULL;
	uint16_t wh_seq = 0;

	if (!nbuf)
		return;

	/* update sequence number in frame header */
	ptr_wh = (struct ieee80211_frame *)qdf_nbuf_data(nbuf);

	wh_seq = (seq_no & 0xFFF) << 4;
	qdf_mem_copy(ptr_wh->i_seq, &wh_seq, sizeof(uint16_t));
}

static inline
void dp_update_frame_ctrl_from_frame_type(void *desc)
{
	struct cdp_tx_completion_ppdu *ppdu_desc = desc;

	/* frame control is not set properly, sometimes it is zero */
	switch (ppdu_desc->htt_frame_type) {
	case HTT_STATS_FTYPE_SGEN_NDPA:
	case HTT_STATS_FTYPE_SGEN_NDP:
	case HTT_STATS_FTYPE_SGEN_AX_NDPA:
	case HTT_STATS_FTYPE_SGEN_AX_NDP:
		ppdu_desc->frame_ctrl = (IEEE80211_FC0_SUBTYPE_NDPA |
					 IEEE80211_FC0_TYPE_CTL);
	break;
	case HTT_STATS_FTYPE_SGEN_BRP:
		ppdu_desc->frame_ctrl = (IEEE80211_FC0_SUBTYPE_BRPOLL |
					 IEEE80211_FC0_TYPE_CTL);
	break;
	case HTT_STATS_FTYPE_SGEN_RTS:
	case HTT_STATS_FTYPE_SGEN_MU_RTS:
		ppdu_desc->frame_ctrl = (IEEE80211_FC0_SUBTYPE_RTS |
					 IEEE80211_FC0_TYPE_CTL);
	break;
	case HTT_STATS_FTYPE_SGEN_CTS:
		ppdu_desc->frame_ctrl = (IEEE80211_FC0_SUBTYPE_CTS |
					 IEEE80211_FC0_TYPE_CTL);
	break;
	case HTT_STATS_FTYPE_SGEN_CFEND:
		ppdu_desc->frame_ctrl = (IEEE80211_FC0_SUBTYPE_CF_END |
					 IEEE80211_FC0_TYPE_CTL);
	break;
	case HTT_STATS_FTYPE_SGEN_MU_TRIG:
	case HTT_STATS_FTYPE_SGEN_MU_BAR:
	case HTT_STATS_FTYPE_SGEN_MU_BRP:
		ppdu_desc->frame_ctrl = (IEEE80211_FC0_SUBTYPE_TRIGGER |
					 IEEE80211_FC0_TYPE_CTL);
	break;
	case HTT_STATS_FTYPE_SGEN_BAR:
		ppdu_desc->frame_ctrl = (IEEE80211_FC0_SUBTYPE_BAR |
					  IEEE80211_FC0_TYPE_CTL);
	break;
	}
}

/**
 * dp_send_dummy_mpdu_info_to_stack(): send dummy payload to stack
 * to upper layer if complete
 * @pdev: DP pdev handle
 * @desc: cdp tx completion ppdu desc
 * @usr_idx: user index
 *
 * return: status
 */
static inline
QDF_STATUS dp_send_dummy_mpdu_info_to_stack(struct dp_pdev *pdev,
					    void *desc, uint8_t usr_idx)
{
	struct cdp_tx_completion_ppdu *ppdu_desc = desc;
	struct cdp_tx_completion_ppdu_user *user = &ppdu_desc->user[usr_idx];
	struct ieee80211_ctlframe_addr2 *wh_min;
	uint16_t frame_ctrl_le, duration_le;
	struct cdp_tx_indication_info tx_capture_info;
	struct cdp_tx_indication_mpdu_info *mpdu_info;
	uint8_t type, subtype;

	qdf_mem_set(&tx_capture_info,
		    sizeof(struct cdp_tx_indication_info),
		    0);

	tx_capture_info.mpdu_nbuf =
		qdf_nbuf_alloc(pdev->soc->osdev,
			       MAX_MONITOR_HEADER + MAX_DUMMY_FRM_BODY,
			       MAX_MONITOR_HEADER,
			       4, FALSE);
	if (!tx_capture_info.mpdu_nbuf)
		return QDF_STATUS_E_ABORTED;

	mpdu_info = &tx_capture_info.mpdu_info;

	mpdu_info->resp_type = ppdu_desc->resp_type;
	mpdu_info->mprot_type = ppdu_desc->mprot_type;
	mpdu_info->rts_success = ppdu_desc->rts_success;
	mpdu_info->rts_failure = ppdu_desc->rts_failure;

	/* update cdp_tx_indication_mpdu_info */
	dp_tx_update_user_mpdu_info(ppdu_desc->bar_ppdu_id,
				    &tx_capture_info.mpdu_info,
				    user);
	tx_capture_info.ppdu_desc = ppdu_desc;

	mpdu_info->ppdu_id = ppdu_desc->ppdu_id;

	mpdu_info->channel_num = pdev->operating_channel.num;
	mpdu_info->channel = ppdu_desc->channel;
	mpdu_info->frame_type = ppdu_desc->frame_type;
	mpdu_info->ppdu_start_timestamp = ppdu_desc->ppdu_start_timestamp;
	mpdu_info->ppdu_end_timestamp = ppdu_desc->ppdu_end_timestamp;
	mpdu_info->tx_duration = ppdu_desc->tx_duration;
	mpdu_info->seq_no = user->start_seq;
	qdf_mem_copy(mpdu_info->mac_address, user->mac_addr, QDF_MAC_ADDR_SIZE);

	mpdu_info->ba_start_seq = user->ba_seq_no;
	qdf_mem_copy(mpdu_info->ba_bitmap, user->ba_bitmap,
		     CDP_BA_256_BIT_MAP_SIZE_DWORDS * sizeof(uint32_t));

	mpdu_info->frame_ctrl = ppdu_desc->frame_ctrl;

	type = (ppdu_desc->frame_ctrl & IEEE80211_FC0_TYPE_MASK);
	subtype = (ppdu_desc->frame_ctrl & IEEE80211_FC0_SUBTYPE_MASK);

	if (type == IEEE80211_FC0_TYPE_CTL &&
	    subtype == IEEE80211_FC0_SUBTYPE_BAR) {
		mpdu_info->frame_ctrl = (IEEE80211_FC0_SUBTYPE_BAR |
					  IEEE80211_FC0_TYPE_CTL);
		mpdu_info->ppdu_id = ppdu_desc->bar_ppdu_id;
		mpdu_info->ppdu_start_timestamp =
					ppdu_desc->bar_ppdu_start_timestamp;
		mpdu_info->ppdu_end_timestamp =
					ppdu_desc->bar_ppdu_end_timestamp;
		mpdu_info->tx_duration = ppdu_desc->bar_tx_duration;
	}

	wh_min = (struct ieee80211_ctlframe_addr2 *)
		qdf_nbuf_data(
		tx_capture_info.mpdu_nbuf);
	qdf_mem_zero(wh_min, MAX_DUMMY_FRM_BODY);
	frame_ctrl_le =
		qdf_cpu_to_le16(mpdu_info->frame_ctrl);
	duration_le =
		qdf_cpu_to_le16(mpdu_info->tx_duration);
	wh_min->i_fc[1] = (frame_ctrl_le & 0xFF00) >> 8;
	wh_min->i_fc[0] = (frame_ctrl_le & 0xFF);
	wh_min->i_aidordur[1] = (duration_le & 0xFF00) >> 8;
	wh_min->i_aidordur[0] = (duration_le & 0xFF);
	qdf_mem_copy(wh_min->i_addr1,
		     mpdu_info->mac_address,
		     QDF_MAC_ADDR_SIZE);

	if (subtype == IEEE80211_FC0_SUBTYPE_ACK)
		qdf_nbuf_set_pktlen(tx_capture_info.mpdu_nbuf,
				    sizeof(struct ieee80211_frame_min_one));
	else {
		struct dp_peer *peer;
		struct dp_vdev *vdev = NULL;

		peer = DP_TX_PEER_GET_REF(pdev, user->peer_id);
		if (peer) {
			vdev = peer->vdev;

			if (vdev)
				qdf_mem_copy(wh_min->i_addr2,
					     vdev->mac_addr.raw,
					     QDF_MAC_ADDR_SIZE);
			DP_TX_PEER_DEL_REF(peer);
		} else {
			vdev =
			dp_vdev_get_ref_by_id(pdev->soc, ppdu_desc->vdev_id,
					      DP_MOD_ID_TX_CAPTURE);
			if (vdev) {
				qdf_mem_copy(wh_min->i_addr2,
					     vdev->mac_addr.raw,
					     QDF_MAC_ADDR_SIZE);
				dp_vdev_unref_delete(pdev->soc, vdev,
						     DP_MOD_ID_TX_CAPTURE);
			}
		}
		qdf_nbuf_set_pktlen(tx_capture_info.mpdu_nbuf, sizeof(*wh_min));
	}

	dp_tx_capture_debug("%pK: HTT_FTYPE[%d] frm(0x%08x): fc %x %x, dur 0x%x%x\n",
			    pdev->soc, ppdu_desc->htt_frame_type,
			    mpdu_info->ppdu_id,
			    wh_min->i_fc[1], wh_min->i_fc[0],
			    wh_min->i_aidordur[1], wh_min->i_aidordur[0]);
	/*
	 * send MPDU to osif layer
	 */
	TX_CAP_WDI_EVENT_HANDLER(pdev->soc, pdev->pdev_id, &tx_capture_info);

	if (tx_capture_info.mpdu_nbuf)
		qdf_nbuf_free(tx_capture_info.mpdu_nbuf);

	return QDF_STATUS_SUCCESS;
}

/**
 * dp_send_dummy_rts_cts_frame(): send dummy rts and cts frame out
 * to upper layer if complete
 * @pdev: DP pdev handle
 * @cur_ppdu_desc: cdp tx completion ppdu desc
 *
 * return: void
 */
static
void dp_send_dummy_rts_cts_frame(struct dp_pdev *pdev,
				 struct cdp_tx_completion_ppdu *cur_ppdu_desc,
				 uint8_t usr_id)
{
	struct cdp_tx_completion_ppdu *ppdu_desc;
	struct dp_pdev_tx_capture *ptr_tx_cap;
	struct dp_peer *peer;
	uint8_t rts_send;
	struct dp_vdev *vdev = NULL;
	struct dp_mon_pdev *mon_pdev = pdev->monitor_pdev;

	rts_send = 0;
	ptr_tx_cap = &mon_pdev->tx_capture;
	ppdu_desc = ptr_tx_cap->dummy_ppdu_desc;

	ppdu_desc->channel = cur_ppdu_desc->channel;
	ppdu_desc->num_mpdu = 1;
	ppdu_desc->num_msdu = 1;
	ppdu_desc->user[usr_id].ppdu_type = HTT_PPDU_STATS_PPDU_TYPE_SU;
	ppdu_desc->bar_num_users = 0;
	ppdu_desc->num_users = 1;

	if (cur_ppdu_desc->mprot_type == SEND_WIFIRTS_LEGACY_E ||
	    cur_ppdu_desc->mprot_type == SEND_WIFIRTS_11AC_DYNAMIC_BW_E ||
	    cur_ppdu_desc->mprot_type == SEND_WIFIRTS_11AC_STATIC_BW_E) {
		rts_send = 1;
		/*
		 *  send dummy RTS frame followed by CTS
		 *  update frame_ctrl and htt_frame_type
		 */
		ppdu_desc->htt_frame_type = HTT_STATS_FTYPE_SGEN_RTS;
		ppdu_desc->frame_type = CDP_PPDU_FTYPE_CTRL;
		ppdu_desc->ppdu_start_timestamp =
				cur_ppdu_desc->ppdu_start_timestamp;
		ppdu_desc->ppdu_end_timestamp =
				cur_ppdu_desc->ppdu_end_timestamp;
		ppdu_desc->tx_duration = cur_ppdu_desc->tx_duration;
		ppdu_desc->user[usr_id].peer_id =
				cur_ppdu_desc->user[usr_id].peer_id;
		ppdu_desc->frame_ctrl = (IEEE80211_FC0_SUBTYPE_RTS |
					 IEEE80211_FC0_TYPE_CTL);
		qdf_mem_copy(&ppdu_desc->user[usr_id].mac_addr,
			     &cur_ppdu_desc->user[usr_id].mac_addr,
			     QDF_MAC_ADDR_SIZE);

		dp_send_dummy_mpdu_info_to_stack(pdev, ppdu_desc, usr_id);
	}

	if ((rts_send && cur_ppdu_desc->rts_success) ||
	    cur_ppdu_desc->mprot_type == SEND_WIFICTS2SELF_E) {
		uint16_t peer_id;

		peer_id = cur_ppdu_desc->user[usr_id].peer_id;
		/* send dummy CTS frame */
		ppdu_desc->htt_frame_type = HTT_STATS_FTYPE_SGEN_CTS;
		ppdu_desc->frame_type = CDP_PPDU_FTYPE_CTRL;
		ppdu_desc->frame_ctrl = (IEEE80211_FC0_SUBTYPE_CTS |
					 IEEE80211_FC0_TYPE_CTL);
		ppdu_desc->ppdu_start_timestamp =
				cur_ppdu_desc->ppdu_start_timestamp;
		ppdu_desc->ppdu_end_timestamp =
				cur_ppdu_desc->ppdu_end_timestamp;
		ppdu_desc->tx_duration = cur_ppdu_desc->tx_duration -
					 (RTS_INTERVAL + SIFS_INTERVAL);
		ppdu_desc->user[usr_id].peer_id = peer_id;
		peer = DP_TX_PEER_GET_REF(pdev, peer_id);
		if (peer) {
			vdev = peer->vdev;
			if (vdev)
				qdf_mem_copy(&ppdu_desc->user[usr_id].mac_addr,
					     vdev->mac_addr.raw,
					     QDF_MAC_ADDR_SIZE);
			DP_TX_PEER_DEL_REF(peer);
		} else {
			uint8_t vdev_id;

			vdev_id = ppdu_desc->vdev_id;
			vdev = dp_vdev_get_ref_by_id(pdev->soc, vdev_id,
						     DP_MOD_ID_TX_CAPTURE);
			if (vdev) {
				qdf_mem_copy(&ppdu_desc->user[usr_id].mac_addr,
					     vdev->mac_addr.raw,
					     QDF_MAC_ADDR_SIZE);
				dp_vdev_unref_delete(pdev->soc, vdev,
						     DP_MOD_ID_TX_CAPTURE);
			}
		}

		dp_send_dummy_mpdu_info_to_stack(pdev, ppdu_desc, usr_id);
	}
}

static void dp_gen_ack_rx_frame(struct dp_pdev *pdev,
				struct cdp_tx_indication_info *tx_capture_info)
{
	struct cdp_tx_completion_ppdu *ppdu_desc;
	struct dp_peer *peer;
	struct dp_pdev_tx_capture *ptr_tx_cap;
	struct dp_mon_pdev *mon_pdev = pdev->monitor_pdev;

	ptr_tx_cap = &mon_pdev->tx_capture;
	ppdu_desc = ptr_tx_cap->dummy_ppdu_desc;
	ppdu_desc->channel = tx_capture_info->ppdu_desc->channel;
	ppdu_desc->num_mpdu = 1;
	ppdu_desc->num_msdu = 1;
	ppdu_desc->user[0].ppdu_type = HTT_PPDU_STATS_PPDU_TYPE_SU;
	ppdu_desc->bar_num_users = 0;
	ppdu_desc->num_users = 1;

	ppdu_desc->frame_type = CDP_PPDU_FTYPE_CTRL;
	ppdu_desc->frame_ctrl = (IEEE80211_FC0_SUBTYPE_ACK |
				 IEEE80211_FC0_TYPE_CTL);
	ppdu_desc->ppdu_start_timestamp =
			tx_capture_info->ppdu_desc->ppdu_start_timestamp;
	ppdu_desc->ppdu_end_timestamp =
			tx_capture_info->ppdu_desc->ppdu_end_timestamp;
	ppdu_desc->user[0].peer_id =
			tx_capture_info->ppdu_desc->user[0].peer_id;
	peer = DP_TX_PEER_GET_REF(pdev,
				  tx_capture_info->ppdu_desc->user[0].peer_id);
	if (peer) {
		struct dp_vdev *vdev = NULL;

		vdev = peer->vdev;
		if (vdev)
			qdf_mem_copy(&ppdu_desc->user[0].mac_addr,
				     vdev->mac_addr.raw,
				     QDF_MAC_ADDR_SIZE);
		DP_TX_PEER_DEL_REF(peer);
	}

	dp_send_dummy_mpdu_info_to_stack(pdev, ppdu_desc, 0);
}

/**
 * dp_send_data_to_stack(): Function to deliver mpdu info to stack
 * to upper layer
 * @pdev: DP pdev handle
 * @nbuf_ppdu_list: ppdu_desc_list per sched cmd id
 * @ppdu_desc_cnt: number of ppdu_desc_cnt
 *
 * return: status
 */
static
void dp_send_data_to_stack(struct dp_pdev *pdev,
			   struct cdp_tx_completion_ppdu *ppdu_desc,
			   uint8_t usr_idx)
{
	struct cdp_tx_completion_ppdu_user *user = NULL;
	struct cdp_tx_indication_info tx_capture_info;
	struct cdp_tx_indication_mpdu_info *mpdu_info;
	int i;
	uint32_t seq_no, start_seq;
	uint32_t ppdu_id;
	uint32_t mpdu_tried;
	uint32_t mpdu_enq = 0;
	struct dp_peer *peer;

	if (!ppdu_desc)
		return;

	ppdu_id = ppdu_desc->ppdu_id;
	user = &ppdu_desc->user[usr_idx];

	peer = DP_TX_PEER_GET_REF(pdev, user->peer_id);
	if (!peer)
		return;

	qdf_mem_set(&tx_capture_info,
		    sizeof(struct cdp_tx_indication_info),
		    0);

	mpdu_info = &tx_capture_info.mpdu_info;

	mpdu_info->usr_idx = usr_idx;
	mpdu_info->channel = ppdu_desc->channel;
	mpdu_info->frame_type = ppdu_desc->frame_type;
	mpdu_info->ppdu_start_timestamp =
				ppdu_desc->ppdu_start_timestamp;
	mpdu_info->ppdu_end_timestamp =
				ppdu_desc->ppdu_end_timestamp;
	mpdu_info->tx_duration = ppdu_desc->tx_duration;
	mpdu_info->num_msdu = ppdu_desc->num_msdu;

	mpdu_info->resp_type = ppdu_desc->resp_type;
	mpdu_info->mprot_type = ppdu_desc->mprot_type;
	mpdu_info->rts_success = ppdu_desc->rts_success;
	mpdu_info->rts_failure = ppdu_desc->rts_failure;

	/* update cdp_tx_indication_mpdu_info */
	dp_tx_update_user_mpdu_info(ppdu_id,
				    &tx_capture_info.mpdu_info,
				    user);
	tx_capture_info.ppdu_desc = ppdu_desc;
	tx_capture_info.mpdu_info.channel_num = pdev->operating_channel.num;

	if (ppdu_desc->mprot_type && (usr_idx == 0))
		dp_send_dummy_rts_cts_frame(pdev, ppdu_desc, usr_idx);

	start_seq = user->start_seq;

	if (!user->mpdus)
		goto return_send_to_stack;

	mpdu_tried = user->mpdu_tried_ucast + user->mpdu_tried_mcast;

	for (i = 0; i < CDP_BA_256_BIT_MAP_SIZE_DWORDS; i++)
		mpdu_enq += get_number_of_1s(user->enq_bitmap[i]);

	if (mpdu_tried > mpdu_enq)
		dp_ppdu_desc_debug_print(ppdu_desc, usr_idx,
					 __func__, __LINE__);

	for (i = 0; i < user->ba_size && mpdu_tried; i++) {
		if (qdf_likely(user->tid != DP_NON_QOS_TID) &&
		    !(SEQ_BIT(user->enq_bitmap, i))) {
			continue;
		}

		mpdu_tried--;

		seq_no = start_seq + i;
		if (!user->mpdus[i])
			continue;

		tx_capture_info.mpdu_nbuf = user->mpdus[i];
		dp_tx_cap_stats_mpdu_update(peer, PEER_MPDU_TO_STACK, 1);
		user->mpdus[i] = NULL;
		mpdu_info->seq_no = seq_no;
		dp_tx_update_sequence_number(tx_capture_info.mpdu_nbuf, seq_no);
		/*
		 * send MPDU to osif layer
		 * do we need to update mpdu_info before tranmit
		 * get current mpdu_nbuf
		 */
		TX_CAP_WDI_EVENT_HANDLER(pdev->soc, pdev->pdev_id,
					 &tx_capture_info);

		if (tx_capture_info.mpdu_nbuf)
			qdf_nbuf_free(tx_capture_info.mpdu_nbuf);
	}

	if (ppdu_desc->resp_type == HTT_PPDU_STATS_ACK_EXPECTED_E &&
	    ppdu_desc->user[usr_idx].completion_status ==
	    HTT_PPDU_STATS_USER_STATUS_OK)
		dp_gen_ack_rx_frame(pdev, &tx_capture_info);

return_send_to_stack:
	DP_TX_PEER_DEL_REF(peer);
}

/**
 * dp_ppdu_desc_free(): Function to free ppdu_desc and stored queue
 * @ptr_nbuf_list: pointer to ptr_nbuf_list
 * @usr_idx: user index
 *
 * return: void
 */
static void dp_ppdu_desc_free(struct dp_pdev *pdev,
			      struct dp_tx_cap_nbuf_list *ptr_nbuf_list,
			      uint8_t usr_idx)
{
	struct cdp_tx_completion_ppdu *ppdu_desc = NULL;
	qdf_nbuf_t tmp_nbuf;

	if (!ptr_nbuf_list->nbuf_ppdu ||
	    !dp_tx_cap_nbuf_list_get_ref(ptr_nbuf_list))
		return;

	tmp_nbuf = ptr_nbuf_list->nbuf_ppdu;

	if (tmp_nbuf) {
		ppdu_desc = (struct cdp_tx_completion_ppdu *)
				qdf_nbuf_data(tmp_nbuf);
		dp_ppdu_queue_free(pdev, tmp_nbuf, usr_idx);
		dp_tx_cap_nbuf_list_dec_ref(ptr_nbuf_list);
		qdf_nbuf_free(tmp_nbuf);
	}
}

/**
 * dp_ppdu_desc_free_all(): Function to free all user in a ppdu_desc and
 * its stored queue
 * @ptr_nbuf_list: pointer to ptr_nbuf_list
 * @max_users: maximum number of users
 *
 * return: void
 */
static void dp_ppdu_desc_free_all(struct dp_pdev *pdev,
				  struct dp_tx_cap_nbuf_list *ptr_nbuf_list,
				  uint8_t max_users)
{
	uint8_t i = 0;

	for (i = 0; i < max_users; i++)
		dp_ppdu_desc_free(pdev, ptr_nbuf_list, i);
}

/**
 * dp_tx_mon_get_next_mpdu(): get next mpdu from retry queue.
 * @tx_tid: tx capture peer tid
 * @xretry_user: pointer to ppdu_desc user.
 * @mpdu_nbuf: mpdu nbuf
 * @ppdu_nbuf: ppdu nbuf from pending ppdu q
 *
 * return: qdf_nbuf_t
 */
static qdf_nbuf_t
dp_tx_mon_get_next_mpdu(struct dp_pdev *pdev, struct dp_tx_tid *tx_tid,
			struct cdp_tx_completion_ppdu_user *xretry_user,
			qdf_nbuf_t mpdu_nbuf, qdf_nbuf_t ppdu_nbuf)
{
	qdf_nbuf_t next_nbuf = NULL;
	qdf_nbuf_queue_t temp_xretries;
	qdf_nbuf_t first_nbuf = NULL;
	qdf_nbuf_t temp_nbuf = NULL;

	if (mpdu_nbuf != qdf_nbuf_queue_first(&xretry_user->mpdu_q)) {
		next_nbuf = qdf_nbuf_queue_next(mpdu_nbuf);
		dp_tx_capture_info("%pK: mpdu %p not head, next %p mpdu_q[%p L %d] ppdu %p",
				   pdev->soc, mpdu_nbuf, next_nbuf,
				   &xretry_user->mpdu_q,
				   qdf_nbuf_queue_len(&xretry_user->mpdu_q), ppdu_nbuf);
		/* Initialize temp list */
		qdf_nbuf_queue_init(&temp_xretries);
		/* Move entries into temp list till the mpdu_nbuf is found */
		first_nbuf = qdf_nbuf_queue_first(&xretry_user->mpdu_q);
		while (first_nbuf && (mpdu_nbuf != first_nbuf)) {
			/* remove nbuf from queue */
			temp_nbuf = qdf_nbuf_queue_remove(&xretry_user->mpdu_q);
			qdf_nbuf_queue_add(&temp_xretries, temp_nbuf);
			/* get first nbuf from queue again */
			first_nbuf = qdf_nbuf_queue_first(&xretry_user->mpdu_q);
		}
		if (first_nbuf && (mpdu_nbuf == first_nbuf)) {
			/* Remove mpdu_nbuf from queue */
			qdf_nbuf_queue_remove(&xretry_user->mpdu_q);
			/* Add remaining nbufs into temp queue */
			qdf_nbuf_queue_append(&temp_xretries,
					      &xretry_user->mpdu_q);
			/* Reinit xretry_user->mpdu_q */
			qdf_nbuf_queue_init(&xretry_user->mpdu_q);
			/* append all the entries into original queue */
			qdf_nbuf_queue_append(&xretry_user->mpdu_q,
					      &temp_xretries);
		} else {
			dp_tx_capture_alert("%pK: bug scenario, did not find nbuf in queue\npdev %p "
					    "peer id %d, tid: %p mpdu_nbuf %p xretry_user %p "
					    "mpdu_q %p len %d temp_xretry %p",
					    pdev->soc, pdev, tx_tid->peer_id,
					    tx_tid, mpdu_nbuf,
					    xretry_user, &xretry_user->mpdu_q,
					    qdf_nbuf_queue_len(&xretry_user->mpdu_q),
					    &temp_xretries);
			qdf_assert_always(0);
		}
	} else {
		qdf_nbuf_queue_remove(&xretry_user->mpdu_q);
		next_nbuf = qdf_nbuf_queue_first(&xretry_user->mpdu_q);
	}

	return next_nbuf;
}

static void
dp_tx_mon_proc_xretries(struct dp_pdev *pdev, struct dp_peer *peer,
			uint16_t tid)
{
	struct dp_tx_tid *tx_tid = &peer->monitor_peer->tx_capture.tx_tid[tid];
	struct cdp_tx_completion_ppdu *ppdu_desc;
	struct cdp_tx_completion_ppdu *xretry_ppdu;
	struct cdp_tx_completion_ppdu_user *user = NULL;
	struct cdp_tx_completion_ppdu_user *xretry_user = NULL;
	qdf_nbuf_t ppdu_nbuf;
	qdf_nbuf_t mpdu_nbuf;
	uint32_t mpdu_tried = 0;
	int i;
	uint32_t seq_no;
	uint8_t usr_idx = 0;

	xretry_ppdu = tx_tid->xretry_ppdu;
	if (!xretry_ppdu) {
		dp_tx_capture_alert("%pK: xretry_ppdu is NULL",
				    pdev->soc);
		return;
	}

	xretry_user = &xretry_ppdu->user[0];
	if (!xretry_user) {
		dp_tx_capture_alert("%pK: xretry_user is NULL",
				    pdev->soc);
		return;
	}

	if (qdf_nbuf_is_queue_empty(&tx_tid->pending_ppdu_q)) {
		TX_CAP_NBUF_QUEUE_FREE(&xretry_user->mpdu_q);
		return;
	}

	if (qdf_nbuf_is_queue_empty(&xretry_user->mpdu_q))
		return;

	ppdu_nbuf = qdf_nbuf_queue_first(&tx_tid->pending_ppdu_q);

	while (ppdu_nbuf) {
		struct msdu_completion_info *ptr_msdu_info = NULL;

		ppdu_desc = (struct cdp_tx_completion_ppdu *)
			qdf_nbuf_data(ppdu_nbuf);

		usr_idx = dp_tx_find_usr_idx_from_peer_id(ppdu_desc,
							  tx_tid->peer_id);

		user = &ppdu_desc->user[usr_idx];

		if (user->pending_retries) {
			uint32_t start_seq = user->start_seq;
			uint32_t index = 0;

			mpdu_tried = user->mpdu_tried_ucast +
				     user->mpdu_tried_mcast;
			mpdu_nbuf = qdf_nbuf_queue_first(&xretry_user->mpdu_q);

			for (i = 0;
			     (i < user->ba_size) &&
			     (mpdu_tried > 0) && mpdu_nbuf;
			     i++) {
				if (!(SEQ_BIT(user->enq_bitmap, i)))
					continue;
				mpdu_tried--;
				/* missed seq number */
				seq_no = start_seq + i;

				if (SEQ_BIT(user->failed_bitmap, i))
					continue;
				dp_tx_capture_info("%pK: fill seqno %d from xretries",
						   pdev->soc, seq_no);

				ptr_msdu_info = (struct msdu_completion_info *)
					(qdf_nbuf_data(qdf_nbuf_get_ext_list(
					mpdu_nbuf)) -
					(sizeof(struct msdu_completion_info) +
					sizeof(qdf_ether_header_t)));
				ptr_msdu_info->transmit_cnt--;
				SEQ_SEG(user->failed_bitmap, i) |=
				SEQ_SEG_MSK(user->failed_bitmap[0], i);
				user->pending_retries--;
				if (ptr_msdu_info->transmit_cnt == 0) {
					index = seq_no - start_seq;
					CHECK_MPDUS_NULL(user->mpdus[index]);
					user->mpdus[index] = mpdu_nbuf;
					dp_tx_cap_stats_mpdu_update(peer,
							PEER_MPDU_ARR, 1);
					/*
					 * This API removes mpdu_nbuf from q
					 * and returns next mpdu from the queue
					 */
					mpdu_nbuf =
						dp_tx_mon_get_next_mpdu(pdev,
								tx_tid,
								xretry_user,
								mpdu_nbuf,
								ppdu_nbuf);
				} else {
					index = seq_no - start_seq;
					CHECK_MPDUS_NULL(user->mpdus[index]);
					user->mpdus[index] =
					qdf_nbuf_copy_expand_fraglist(
						mpdu_nbuf,
						MAX_MONITOR_HEADER, 0);
					dp_tx_cap_stats_mpdu_update(peer,
							PEER_MPDU_CLONE, 1);
					mpdu_nbuf =
						qdf_nbuf_queue_next(mpdu_nbuf);
				}
			}
		}

		if ((user->pending_retries == 0) &&
		    (ppdu_nbuf ==
		     qdf_nbuf_queue_first(&tx_tid->pending_ppdu_q))) {
			qdf_nbuf_queue_remove(&tx_tid->pending_ppdu_q);
			/* Deliver PPDU */
			dp_send_data_to_stack(pdev, ppdu_desc, usr_idx);
			dp_ppdu_queue_free(pdev, ppdu_nbuf, usr_idx);
			qdf_nbuf_free(ppdu_nbuf);
			ppdu_nbuf = qdf_nbuf_queue_first(
				&tx_tid->pending_ppdu_q);
		} else {
			ppdu_nbuf = qdf_nbuf_queue_next(ppdu_nbuf);
		}
	}

	TX_CAP_NBUF_QUEUE_FREE(&xretry_user->mpdu_q);
}

static
struct cdp_tx_completion_ppdu *
check_subseq_ppdu_to_pending_q(struct dp_tx_cap_nbuf_list nbuf_ppdu_list[],
			       uint32_t ppdu_desc_cnt,
			       uint32_t *ppdu_cnt,
			       qdf_nbuf_queue_t *head_ppdu,
			       uint32_t peer_id, uint32_t cur_last_seq,
			       bool last_pend_ppdu)
{
	struct cdp_tx_completion_ppdu *next_ppdu = NULL;
	struct cdp_tx_completion_ppdu_user *next_user;
	struct dp_tx_cap_nbuf_list *ptr_nbuf_list = NULL;
	uint8_t cur_usr_idx;

	while (*ppdu_cnt < (ppdu_desc_cnt - 1)) {
		(*ppdu_cnt)++;
		ptr_nbuf_list = &nbuf_ppdu_list[*ppdu_cnt];

		if (!ptr_nbuf_list->nbuf_ppdu ||
		    !dp_tx_cap_nbuf_list_get_ref(ptr_nbuf_list))
			continue;

		next_ppdu = (struct cdp_tx_completion_ppdu *)
				qdf_nbuf_data(ptr_nbuf_list->nbuf_ppdu);

		if (!next_ppdu)
			continue;

		cur_usr_idx = dp_tx_find_usr_idx_from_peer_id(next_ppdu,
							      peer_id);
		next_user = &next_ppdu->user[cur_usr_idx];

		if ((next_user->skip == 1) || (peer_id != next_user->peer_id) ||
		    (next_user->mon_procd == 1))
			continue;

		if (last_pend_ppdu) {
			qdf_nbuf_t tmp_pend_nbuf;
			uint32_t ppdu_ref_cnt;

			/*
			 * get reference count if it
			 * more than one do clone and
			 * add that to head_ppdu
			 */
			ppdu_ref_cnt =
				dp_tx_cap_nbuf_list_get_ref(ptr_nbuf_list);

			if (ppdu_ref_cnt == 1) {
				tmp_pend_nbuf = ptr_nbuf_list->nbuf_ppdu;
			} else {
				tmp_pend_nbuf = qdf_nbuf_clone(
						ptr_nbuf_list->nbuf_ppdu);
				if (qdf_unlikely(!tmp_pend_nbuf)) {
					qdf_assert_always(0);
					continue;
				}
				qdf_nbuf_free(ptr_nbuf_list->nbuf_ppdu);
			}

			qdf_nbuf_queue_add(head_ppdu, tmp_pend_nbuf);

			/* decrement reference */
			dp_tx_cap_nbuf_list_dec_ref(ptr_nbuf_list);

			next_user->mon_procd = 1;
		}

		if (next_user->last_enq_seq > cur_last_seq)
			return next_ppdu;
	}
	return NULL;
}

#define MAX_PENDING_PPDUS 32
static void
dp_tx_mon_proc_pending_ppdus(struct dp_pdev *pdev, struct dp_tx_tid *tx_tid,
			     struct dp_tx_cap_nbuf_list nbuf_ppdu_list[],
			     uint32_t ppdu_desc_cnt,
			     qdf_nbuf_queue_t *head_ppdu,
			     uint32_t peer_id, uint8_t cur_usr_idx)
{
	struct cdp_tx_completion_ppdu *ppdu_desc = NULL;
	struct cdp_tx_completion_ppdu *cur_ppdu_desc = NULL;
	struct cdp_tx_completion_ppdu_user *user = NULL;
	struct cdp_tx_completion_ppdu_user *cur_user = NULL;
	struct dp_tx_cap_nbuf_list *ptr_nbuf_list = NULL;
	qdf_nbuf_t pend_ppdu;
	uint32_t ppdu_cnt;
	uint32_t failed_seq;
	uint32_t cur_index, cur_start_seq, cur_last_seq;
	int i, k;
	bool last_pend_ppdu = false;
	uint8_t usr_idx;
	struct dp_soc *soc = NULL;

	soc = pdev->soc;

	pend_ppdu = qdf_nbuf_queue_first(&tx_tid->pending_ppdu_q);
	if (!pend_ppdu) {
		for (ppdu_cnt = 0; ppdu_cnt < ppdu_desc_cnt; ppdu_cnt++) {
			ptr_nbuf_list = &nbuf_ppdu_list[ppdu_cnt];

			if (!dp_tx_cap_nbuf_list_get_ref(ptr_nbuf_list)) {
				if (ptr_nbuf_list->nbuf_ppdu)
					qdf_assert_always(0);
				continue;
			}

			ppdu_desc = (struct cdp_tx_completion_ppdu *)
				qdf_nbuf_data(ptr_nbuf_list->nbuf_ppdu);

			if (!ppdu_desc)
				continue;

			user = &ppdu_desc->user[cur_usr_idx];

			if ((user->skip == 1) || (peer_id != user->peer_id) ||
			    (tx_tid->tid != user->tid) ||
			    (user->mon_procd == 1))
				continue;

			if ((user->pending_retries == 0) &&
			    qdf_nbuf_is_queue_empty(&tx_tid->pending_ppdu_q) &&
			    qdf_nbuf_is_queue_empty(head_ppdu)) {
				dp_send_data_to_stack(pdev, ppdu_desc,
						      cur_usr_idx);
				user->mon_procd = 1;
				/* free ppdu_desc from list */
				dp_ppdu_desc_free(pdev, ptr_nbuf_list, cur_usr_idx);
			} else {
				qdf_nbuf_t tmp_pend_nbuf;
				uint32_t ppdu_ref_cnt;

				/*
				 * get reference count if it more than one
				 * do clone and add that to head_ppdu
				 */
				ppdu_ref_cnt =
				dp_tx_cap_nbuf_list_get_ref(ptr_nbuf_list);
				if (ppdu_ref_cnt == 1) {
					tmp_pend_nbuf =
						ptr_nbuf_list->nbuf_ppdu;
				} else {
					tmp_pend_nbuf =
						qdf_nbuf_clone(
						ptr_nbuf_list->nbuf_ppdu);
					if (qdf_unlikely(!tmp_pend_nbuf)) {
						qdf_assert_always(0);
						continue;
					}
					/*
					 * free ppdu_desc to
					 * decrease reference
					 */
					qdf_nbuf_free(ptr_nbuf_list->nbuf_ppdu);
				}

				qdf_nbuf_queue_add(head_ppdu, tmp_pend_nbuf);
				/* decrement reference */
				dp_tx_cap_nbuf_list_dec_ref(ptr_nbuf_list);
				user->mon_procd = 1;
			}
		}
		return;
	}
	while (pend_ppdu) {
		qdf_nbuf_t mpdu_nbuf;
		uint32_t mpdu_tried = 0;

		/* Find missing mpdus from current schedule list */
		ppdu_cnt = 0;
		while (ppdu_cnt < ppdu_desc_cnt) {
			uint8_t idx = 0;

			cur_user = NULL;

			ptr_nbuf_list = &nbuf_ppdu_list[ppdu_cnt];
			ppdu_cnt++;

			if (!dp_tx_cap_nbuf_list_get_ref(ptr_nbuf_list))
				continue;

			cur_ppdu_desc = (struct cdp_tx_completion_ppdu *)
				qdf_nbuf_data(ptr_nbuf_list->nbuf_ppdu);

			if (!cur_ppdu_desc)
				continue;

			for (idx = 0; idx < cur_ppdu_desc->num_users; idx++) {
				if (peer_id ==
				    cur_ppdu_desc->user[idx].peer_id) {
					cur_user = &cur_ppdu_desc->user[idx];
					cur_usr_idx = idx;
					break;
				}
			}

			if (qdf_unlikely(!cur_user))
				continue;

			if ((cur_user->skip == 1) ||
			    (cur_user->mon_procd == 1))
				continue;

			/* to handle last ppdu case we need to decrement */
			ppdu_cnt--;
			break;
		}

		if (ppdu_cnt == ppdu_desc_cnt)
			break;

		if (qdf_unlikely(!cur_user))
			break;

		ppdu_desc = (struct cdp_tx_completion_ppdu *)qdf_nbuf_data(
			pend_ppdu);

		usr_idx = dp_tx_find_usr_idx_from_peer_id(ppdu_desc,
							  peer_id);
		user = &ppdu_desc->user[usr_idx];

		if (pend_ppdu == qdf_nbuf_queue_last(
		    &tx_tid->pending_ppdu_q)) {
			qdf_nbuf_t tmp_pend_nbuf;
			uint32_t ppdu_ref_cnt;

			last_pend_ppdu = true;
			/*
			 * get reference count if it more than one
			 * do clone and add that to head_ppdu
			 */
			ppdu_ref_cnt =
				dp_tx_cap_nbuf_list_get_ref(ptr_nbuf_list);
			if (ppdu_ref_cnt == 1) {
				tmp_pend_nbuf =
					ptr_nbuf_list->nbuf_ppdu;
			} else {
				tmp_pend_nbuf = qdf_nbuf_clone(
						ptr_nbuf_list->nbuf_ppdu);
				if (qdf_unlikely(!tmp_pend_nbuf)) {
					qdf_assert_always(0);
					break;
				}
				qdf_nbuf_free(ptr_nbuf_list->nbuf_ppdu);
			}

			qdf_nbuf_queue_add(head_ppdu, tmp_pend_nbuf);
			/* decrement reference */
			dp_tx_cap_nbuf_list_dec_ref(ptr_nbuf_list);
			cur_user->mon_procd = 1;
		}
		cur_index = 0;
		cur_start_seq = cur_user->start_seq;
		cur_last_seq = cur_user->last_enq_seq;
		if (qdf_unlikely(user->ba_size >
		    CDP_BA_256_BIT_MAP_SIZE_DWORDS *
		    SEQ_SEG_SZ_BITS(user->failed_bitmap))) {
			dp_ppdu_desc_debug_print(ppdu_desc, usr_idx,
						 __func__, __LINE__);
			qdf_assert_always(0);
			return;
		}

		/* mpdu tried */
		mpdu_tried = user->mpdu_tried_mcast + user->mpdu_tried_ucast;

		for (i = 0; (i < user->ba_size) && cur_ppdu_desc &&
		     mpdu_tried && cur_index < cur_user->ba_size; i++) {
			if (!(i & (SEQ_SEG_SZ_BITS(user->failed_bitmap) - 1))) {
				k = SEQ_SEG_INDEX(user->failed_bitmap, i);
				failed_seq = user->failed_bitmap[k] ^
					     user->enq_bitmap[k];
			}

			if (SEQ_BIT(user->enq_bitmap, i))
				mpdu_tried--;

			/* Skip to next bitmap segment if there are no
			 * more holes in current segment
			 */
			if (!failed_seq) {
				i = ((k + 1) *
				SEQ_SEG_SZ_BITS(user->failed_bitmap)) - 1;
				continue;
			}
			if (!(SEQ_SEG_BIT(failed_seq, i)))
				continue;
			failed_seq ^= SEQ_SEG_MSK(failed_seq, i);

			if (!cur_user->mpdus) {
				dp_tx_capture_info("%pK:  peer_id:%d usr_idx:%d cur_usr_idx:%d cur_usr_peer_id:%d\n",
						   pdev->soc,
						   peer_id, usr_idx,
						   cur_usr_idx,
						   cur_user->peer_id);
				continue;
			}
			mpdu_nbuf = cur_user->mpdus[cur_index];
			if (mpdu_nbuf) {
				struct dp_peer *peer;

				dp_tx_capture_info("%pK: fill seqno %d (%d) from swretries",
						   pdev->soc,
						   user->start_seq + i,
						   ppdu_desc->ppdu_id);
				CHECK_MPDUS_NULL(user->mpdus[i]);
				user->mpdus[i] =
				qdf_nbuf_copy_expand_fraglist(
					mpdu_nbuf, MAX_MONITOR_HEADER, 0);

				peer = DP_TX_PEER_GET_REF(pdev, user->peer_id);
				if (peer) {
					dp_tx_cap_stats_mpdu_update(peer,
							PEER_MPDU_CLONE, 1);
					DP_TX_PEER_DEL_REF(peer);
				}
				user->failed_bitmap[k] |=
				SEQ_SEG_MSK(user->failed_bitmap[k], i);
				user->pending_retries--;
			}

			cur_index++;
			if (cur_index >= cur_user->ba_size) {
				dp_tx_capture_info("%pK: ba_size[%d] cur_index[%d]\n",
						   pdev->soc,
						   cur_user->ba_size,
						   cur_index);
				break;
			}

			/* Skip through empty slots in current PPDU */
			while (!(SEQ_BIT(cur_user->enq_bitmap, cur_index))) {
				cur_index++;
				if (cur_index <= (cur_last_seq - cur_start_seq))
					continue;
				cur_ppdu_desc = NULL;

				/*
				 * Check if subsequent PPDUs in this schedule
				 * has higher sequence numbers enqueued
				 */
				cur_ppdu_desc = check_subseq_ppdu_to_pending_q(
								nbuf_ppdu_list,
								ppdu_desc_cnt,
								&ppdu_cnt,
								head_ppdu,
								peer_id,
								cur_last_seq,
								last_pend_ppdu);
				if (!cur_ppdu_desc)
					break;

				cur_usr_idx = dp_tx_find_usr_idx_from_peer_id(
						cur_ppdu_desc, peer_id);

				cur_user = &cur_ppdu_desc->user[cur_usr_idx];

				/* Start from seq. no following cur_last_seq
				 * since everything before is already populated
				 * from previous PPDU
				 */
				cur_start_seq = cur_user->start_seq;
				cur_index = (cur_last_seq >= cur_start_seq) ?
					cur_last_seq - cur_start_seq + 1 : 0;
				cur_last_seq = cur_user->last_enq_seq;
			}
		}
		if ((pend_ppdu ==
		    qdf_nbuf_queue_first(&tx_tid->pending_ppdu_q)) &&
		    (user->pending_retries == 0)) {
			qdf_nbuf_queue_remove(&tx_tid->pending_ppdu_q);
			dp_send_data_to_stack(pdev, ppdu_desc, usr_idx);
			dp_ppdu_queue_free(pdev, pend_ppdu, usr_idx);
			qdf_nbuf_free(pend_ppdu);
			pend_ppdu = qdf_nbuf_queue_first(
				&tx_tid->pending_ppdu_q);
		} else {
			pend_ppdu = qdf_nbuf_queue_next(pend_ppdu);
		}
	}
}

static QDF_STATUS
dp_send_mgmt_ctrl_to_stack(struct dp_pdev *pdev,
			   qdf_nbuf_t nbuf_ppdu_desc,
			   struct cdp_tx_indication_info *ptr_tx_cap_info,
			   qdf_nbuf_t mgmt_ctl_nbuf,
			   bool is_payload)
{
	struct cdp_tx_completion_ppdu *ppdu_desc;
	struct cdp_tx_completion_ppdu_user *user;
	struct cdp_tx_indication_mpdu_info *mpdu_info;
	struct ieee80211_frame *wh;
	uint16_t duration_le, seq_le;
	struct ieee80211_frame_min_one *wh_min;
	uint16_t frame_ctrl_le;
	uint8_t type, subtype;
	QDF_STATUS status = QDF_STATUS_E_FAILURE;

	mpdu_info = &ptr_tx_cap_info->mpdu_info;
	ppdu_desc = (struct cdp_tx_completion_ppdu *)
			qdf_nbuf_data(nbuf_ppdu_desc);
	user = &ppdu_desc->user[0];

	/*
	 * only for the packets send over the air are handled
	 * packets drop by firmware is not handled in this
	 * feature
	 */
	if (user->completion_status == HTT_PPDU_STATS_USER_STATUS_FILTERED) {
		status = QDF_STATUS_SUCCESS;
		qdf_nbuf_free(nbuf_ppdu_desc);

		/* free mgmt_ctl_nbuf only if it is available */
		if (mgmt_ctl_nbuf)
			qdf_nbuf_free(mgmt_ctl_nbuf);
		goto free_mpdu_nbuf;
	}

	if (ppdu_desc->mprot_type)
		dp_send_dummy_rts_cts_frame(pdev, ppdu_desc, 0);

	type = (ppdu_desc->frame_ctrl &
		IEEE80211_FC0_TYPE_MASK) >>
		IEEE80211_FC0_TYPE_SHIFT;
	subtype = (ppdu_desc->frame_ctrl &
		IEEE80211_FC0_SUBTYPE_MASK) >>
		IEEE80211_FC0_SUBTYPE_SHIFT;

	if (is_payload) {
		wh = (struct ieee80211_frame *)qdf_nbuf_data(mgmt_ctl_nbuf);

		if (subtype != IEEE80211_FC0_SUBTYPE_BEACON) {
			duration_le = qdf_cpu_to_le16(ppdu_desc->tx_duration);
			wh->i_dur[1] = (duration_le & 0xFF00) >> 8;
			wh->i_dur[0] = duration_le & 0xFF;
			seq_le = qdf_cpu_to_le16(user->start_seq <<
						 IEEE80211_SEQ_SEQ_SHIFT);
			if (user->is_seq_num_valid) {
				wh->i_seq[1] = (seq_le & 0xFF00) >> 8;
				wh->i_seq[0] = seq_le & 0xFF;
			}
		}
		dp_tx_capture_debug("%pK: ctrl/mgmt frm(0x%08x): fc 0x%x 0x%x\n",
				    pdev->soc,
				    ptr_tx_cap_info->mpdu_info.ppdu_id,
				    wh->i_fc[1], wh->i_fc[0]);
		dp_tx_capture_debug("%pK: desc->ppdu_id 0x%08x\n", pdev->soc, ppdu_desc->ppdu_id);

		/* append ext list */
		qdf_nbuf_append_ext_list(ptr_tx_cap_info->mpdu_nbuf,
					 mgmt_ctl_nbuf,
					 qdf_nbuf_len(mgmt_ctl_nbuf));
	} else {
		wh_min = (struct ieee80211_frame_min_one *)
				qdf_nbuf_data(ptr_tx_cap_info->mpdu_nbuf);
		qdf_mem_zero(wh_min, MAX_DUMMY_FRM_BODY);
		frame_ctrl_le = qdf_cpu_to_le16(ppdu_desc->frame_ctrl);
		duration_le = qdf_cpu_to_le16(ppdu_desc->tx_duration);
		wh_min->i_fc[1] = (frame_ctrl_le & 0xFF00) >> 8;
		wh_min->i_fc[0] = (frame_ctrl_le & 0xFF);
		wh_min->i_dur[1] = (duration_le & 0xFF00) >> 8;
		wh_min->i_dur[0] = (duration_le & 0xFF);
		qdf_mem_copy(wh_min->i_addr1, mpdu_info->mac_address,
			     QDF_MAC_ADDR_SIZE);
		qdf_nbuf_set_pktlen(ptr_tx_cap_info->mpdu_nbuf,
				    sizeof(*wh_min));
		dp_tx_capture_debug("%pK: frm(0x%08x): fc %x %x, dur 0x%x%x\n",
				    pdev->soc,
				    ptr_tx_cap_info->mpdu_info.ppdu_id,
				    wh_min->i_fc[1], wh_min->i_fc[0],
				    wh_min->i_dur[1], wh_min->i_dur[0]);
	}

	TX_CAP_WDI_EVENT_HANDLER(pdev->soc, pdev->pdev_id, ptr_tx_cap_info);

	status = QDF_STATUS_SUCCESS;
	qdf_nbuf_free(nbuf_ppdu_desc);

free_mpdu_nbuf:
	if (ptr_tx_cap_info->mpdu_nbuf)
		qdf_nbuf_free(ptr_tx_cap_info->mpdu_nbuf);

	return status;
}

static uint32_t
dp_update_tx_cap_info(struct dp_pdev *pdev,
		      qdf_nbuf_t nbuf_ppdu_desc,
		      void *tx_info, bool is_payload,
		      bool bar_frm_with_data)
{
	struct cdp_tx_completion_ppdu *ppdu_desc;
	struct cdp_tx_completion_ppdu_user *user;
	struct cdp_tx_indication_info *tx_capture_info =
		(struct cdp_tx_indication_info *)tx_info;
	struct cdp_tx_indication_mpdu_info *mpdu_info;

	ppdu_desc = (struct cdp_tx_completion_ppdu *)
			qdf_nbuf_data(nbuf_ppdu_desc);
	user = &ppdu_desc->user[0];

	qdf_mem_set(tx_capture_info, sizeof(struct cdp_tx_indication_info), 0);
	mpdu_info = &tx_capture_info->mpdu_info;

	mpdu_info->channel = ppdu_desc->channel;
	mpdu_info->frame_type = ppdu_desc->frame_type;
	mpdu_info->ppdu_start_timestamp = ppdu_desc->ppdu_start_timestamp;
	mpdu_info->ppdu_end_timestamp = ppdu_desc->ppdu_end_timestamp;
	mpdu_info->tx_duration = ppdu_desc->tx_duration;

	/* update cdp_tx_indication_mpdu_info */
	dp_tx_update_user_mpdu_info(ppdu_desc->ppdu_id,
				    &tx_capture_info->mpdu_info,
				    user);

	if (bar_frm_with_data) {
		mpdu_info->ppdu_start_timestamp =
			ppdu_desc->bar_ppdu_start_timestamp;
		mpdu_info->ppdu_end_timestamp =
			ppdu_desc->bar_ppdu_end_timestamp;
		mpdu_info->tx_duration = ppdu_desc->bar_tx_duration;
		mpdu_info->preamble = ppdu_desc->phy_mode;
	}

	mpdu_info->seq_no = user->start_seq;
	mpdu_info->num_msdu = ppdu_desc->num_msdu;
	tx_capture_info->ppdu_desc = ppdu_desc;
	tx_capture_info->mpdu_info.channel_num = pdev->operating_channel.num;

	tx_capture_info->mpdu_info.ppdu_id = ppdu_desc->ppdu_id;
	if (is_payload)
		tx_capture_info->mpdu_nbuf = qdf_nbuf_alloc(pdev->soc->osdev,
							    MAX_MONITOR_HEADER,
							    MAX_MONITOR_HEADER,
							    4, FALSE);
	else
		tx_capture_info->mpdu_nbuf = qdf_nbuf_alloc(pdev->soc->osdev,
							    MAX_MONITOR_HEADER +
							    MAX_DUMMY_FRM_BODY,
							    MAX_MONITOR_HEADER,
							    4, FALSE);
	return 0;
}

/**
 * dp_check_mgmt_ctrl_ppdu(): Function to correlate payload to ppdu_desc and
 * send to above layer.
 * @pdev: DP pdev handle
 * @nbuf_ppdu_desc: qdf_nbuf_t ppdu_desc
 * @bar_frm_with_data: flag for bar frame with data
 *
 * return: QDF_STATUS_SUCCESS - ppdu desc is free inside
 * QDF_STATUS_E_FAILURE - on ppdu desc not free yet.
 */
static QDF_STATUS
dp_check_mgmt_ctrl_ppdu(struct dp_pdev *pdev,
			qdf_nbuf_t nbuf_ppdu_desc, bool bar_frm_with_data)
{
	struct cdp_tx_indication_info tx_capture_info;
	qdf_nbuf_t mgmt_ctl_nbuf, tmp_nbuf;
	uint8_t type, subtype;
	uint8_t fc_type, fc_subtype;
	bool is_sgen_pkt;
	struct cdp_tx_mgmt_comp_info *ptr_comp_info;
	qdf_nbuf_queue_t *retries_q;
	struct cdp_tx_completion_ppdu *ppdu_desc, *retry_ppdu;
	struct cdp_tx_completion_ppdu_user *user;
	uint32_t ppdu_id;
	uint32_t desc_ppdu_id;
	size_t head_size;
	uint64_t tsf_delta;
	uint64_t start_tsf;
	uint64_t end_tsf;
	uint16_t ppdu_desc_frame_ctrl;
	struct dp_peer *peer;
	QDF_STATUS status = QDF_STATUS_E_FAILURE;
	struct dp_mon_pdev *mon_pdev = pdev->monitor_pdev;
	struct dp_mon_soc *mon_soc = pdev->soc->monitor_soc;
	bool mem_limit_flag =
		wlan_cfg_get_tx_capt_max_mem(pdev->soc->wlan_cfg_ctx);

	ppdu_desc = (struct cdp_tx_completion_ppdu *)
		qdf_nbuf_data(nbuf_ppdu_desc);

	user = &ppdu_desc->user[0];

	ppdu_desc_frame_ctrl = ppdu_desc->frame_ctrl;
	if ((ppdu_desc->htt_frame_type == HTT_STATS_FTYPE_SGEN_MU_BAR) ||
	    (ppdu_desc->htt_frame_type == HTT_STATS_FTYPE_SGEN_MU_BRP))
		ppdu_desc_frame_ctrl = (IEEE80211_FC0_SUBTYPE_TRIGGER |
					IEEE80211_FC0_TYPE_CTL);

	if (bar_frm_with_data) {
		desc_ppdu_id = ppdu_desc->bar_ppdu_id;
		start_tsf = ppdu_desc->bar_ppdu_start_timestamp;
		end_tsf = ppdu_desc->bar_ppdu_end_timestamp;
	} else {
		desc_ppdu_id = ppdu_desc->ppdu_id;
		start_tsf = ppdu_desc->ppdu_start_timestamp;
		end_tsf = ppdu_desc->ppdu_end_timestamp;
	}

	/*
	 * only for host generated frame we do have
	 * timestamp and retries count.
	 */
	head_size = sizeof(struct cdp_tx_mgmt_comp_info);

	fc_type = (ppdu_desc_frame_ctrl &
		  IEEE80211_FC0_TYPE_MASK);
	fc_subtype = (ppdu_desc_frame_ctrl &
		     IEEE80211_FC0_SUBTYPE_MASK);

	type = (ppdu_desc_frame_ctrl &
		IEEE80211_FC0_TYPE_MASK) >>
		IEEE80211_FC0_TYPE_SHIFT;
	subtype = (ppdu_desc_frame_ctrl &
		IEEE80211_FC0_SUBTYPE_MASK) >>
		IEEE80211_FC0_SUBTYPE_SHIFT;

	if (ppdu_desc->htt_frame_type == HTT_STATS_FTYPE_SGEN_NDP) {
		dp_update_frame_ctrl_from_frame_type(ppdu_desc);
		type = 0;
		subtype = 0;
	}

	peer = DP_TX_PEER_GET_REF(pdev, ppdu_desc->user[0].peer_id);
	if (peer && !peer->bss_peer) {
		if (!dp_peer_or_pdev_tx_cap_enabled(pdev, peer,
						    ppdu_desc->user[0].mac_addr
						    )) {
			qdf_nbuf_free(nbuf_ppdu_desc);
			status = QDF_STATUS_SUCCESS;
			DP_TX_PEER_DEL_REF(peer);
			goto exit;
		}
		DP_TX_PEER_DEL_REF(peer);
	} else {
		if (peer)
			DP_TX_PEER_DEL_REF(peer);
		if (!(type == IEEE80211_FC0_TYPE_MGT &&
		      (subtype == MGMT_SUBTYPE_PROBE_RESP >> 4 ||
		       subtype == MGMT_SUBTYPE_DISASSOC >> 4 ||
		       subtype == MGMT_SUBTYPE_DEAUTH >> 4 ||
		       subtype == MGMT_SUBTYPE_AUTH >> 4))) {
			if (!dp_peer_or_pdev_tx_cap_enabled(pdev, NULL,
							    ppdu_desc->user[0]
							    .mac_addr)) {
				qdf_nbuf_free(nbuf_ppdu_desc);
				status = QDF_STATUS_SUCCESS;
				goto exit;
			}
		}
	}

	switch (ppdu_desc->htt_frame_type) {
	case HTT_STATS_FTYPE_TIDQ_DATA_SU:
	case HTT_STATS_FTYPE_TIDQ_DATA_MU:
		if ((fc_type == IEEE80211_FC0_TYPE_MGT) &&
		    (fc_subtype == IEEE80211_FC0_SUBTYPE_BEACON))
			is_sgen_pkt = true;
		else
			is_sgen_pkt = false;
	break;
	default:
		is_sgen_pkt = true;
	break;
	}

	retries_q = &mon_pdev->tx_capture.retries_ctl_mgmt_q[type][subtype];

	if (!qdf_nbuf_is_queue_empty(retries_q)) {
		tmp_nbuf  = qdf_nbuf_queue_first(retries_q);
		retry_ppdu = (struct cdp_tx_completion_ppdu *)
			      qdf_nbuf_data(tmp_nbuf);

		if (ppdu_desc->sched_cmdid != retry_ppdu->sched_cmdid) {
			if (mem_limit_flag) {
				uint32_t nbytes = get_queue_bytes(retries_q);
				qdf_atomic_sub(nbytes,
					       &mon_soc->dp_soc_tx_capt.ppdu_mgmt_bytes);
			}
			TX_CAP_NBUF_QUEUE_FREE(retries_q);
		}
	}

get_mgmt_pkt_from_queue:
	qdf_spin_lock_bh(
		&mon_pdev->tx_capture.ctl_mgmt_lock[type][subtype]);
	mgmt_ctl_nbuf = qdf_nbuf_queue_remove(
		&mon_pdev->tx_capture.ctl_mgmt_q[type][subtype]);
	qdf_spin_unlock_bh(&mon_pdev->tx_capture.ctl_mgmt_lock[type][subtype]);

	if (mgmt_ctl_nbuf) {
		qdf_nbuf_t tmp_mgmt_ctl_nbuf;

		if (mem_limit_flag) {
			qdf_atomic_sub(qdf_nbuf_get_truesize(mgmt_ctl_nbuf),
				       &mon_soc->dp_soc_tx_capt.ppdu_mgmt_bytes);
		}

		ptr_comp_info = (struct cdp_tx_mgmt_comp_info *)
				qdf_nbuf_data(mgmt_ctl_nbuf);
		is_sgen_pkt = ptr_comp_info->is_sgen_pkt;
		ppdu_id = ptr_comp_info->ppdu_id;

		if (!is_sgen_pkt && ptr_comp_info->tx_tsf < start_tsf) {
			/*
			 * free the older mgmt buffer from
			 * the queue and get new mgmt buffer
			 */
			qdf_nbuf_free(mgmt_ctl_nbuf);
			goto get_mgmt_pkt_from_queue;
		}

		/*
		 * for sgen frame we won't have, retries count
		 * and 64 bits tsf in the head.
		 */
		if (ppdu_id != desc_ppdu_id) {
			if (is_sgen_pkt) {
				start_tsf = (start_tsf & LOWER_32_MASK);
				if (start_tsf > ptr_comp_info->tx_tsf)
					tsf_delta = start_tsf -
						ptr_comp_info->tx_tsf;
				else
					tsf_delta = LOWER_32_MASK -
						ptr_comp_info->tx_tsf +
						start_tsf;

				dp_tx_capture_info("%pK: ppdu_id[m:%d desc:%d] start_tsf: %llu mgmt_tsf:%llu tsf_delta:%llu bar_frm_with_data:%d",
						   pdev->soc,
						   ppdu_id,
						   desc_ppdu_id,
						   start_tsf,
						   ptr_comp_info->tx_tsf,
						   tsf_delta,
						   bar_frm_with_data);

				if (tsf_delta > MAX_MGMT_ENQ_DELAY) {
					/*
					 * free the older mgmt buffer from
					 * the queue and get new mgmt buffer
					 */
					qdf_nbuf_free(mgmt_ctl_nbuf);
					goto get_mgmt_pkt_from_queue;
				} else {
					/* drop the ppdu_desc */
					qdf_nbuf_free(nbuf_ppdu_desc);
					status = QDF_STATUS_SUCCESS;
					goto insert_mgmt_buf_to_queue;
				}
			}

			/*
			 * only for the packets send over the air are handled
			 * packets drop by firmware is not handled in this
			 * feature
			 */
			if (user->completion_status ==
			    HTT_PPDU_STATS_USER_STATUS_FILTERED) {
				qdf_nbuf_free(nbuf_ppdu_desc);
				status = QDF_STATUS_SUCCESS;
				goto insert_mgmt_buf_to_queue;
			}

			/* check for max retry count */
			if (qdf_nbuf_queue_len(retries_q) >=
			    MAX_RETRY_Q_COUNT) {
				qdf_nbuf_t nbuf_retry_ppdu;

				nbuf_retry_ppdu =
					qdf_nbuf_queue_remove(retries_q);

				if (mem_limit_flag) {
					if (qdf_likely(nbuf_retry_ppdu)) {
						qdf_atomic_sub(qdf_nbuf_get_truesize(nbuf_retry_ppdu),
								   &mon_soc->dp_soc_tx_capt.ppdu_mgmt_bytes);
					}
				}
				qdf_nbuf_free(nbuf_retry_ppdu);
			}

			/* if adding the new buffer goes beyond the allowed limit,
			 * drop the buffer
			 */
			if (mem_limit_flag) {
				if (!dp_tx_capt_mem_check(pdev,
							  qdf_nbuf_get_truesize(nbuf_ppdu_desc))) {
					mon_soc->dp_soc_tx_capt.mem_limit_drops++;
					qdf_nbuf_free(nbuf_ppdu_desc);
				} else {
					qdf_atomic_add(qdf_nbuf_get_truesize(nbuf_ppdu_desc),
						       &mon_soc->dp_soc_tx_capt.ppdu_mgmt_bytes);
					qdf_nbuf_queue_add(retries_q, nbuf_ppdu_desc);
				}
			} else {
				/*
				 * add the ppdu_desc into retry queue
				 */
				qdf_nbuf_queue_add(retries_q, nbuf_ppdu_desc);
			}

			/* flushing retry queue since completion status is
			 * in final state. meaning that even though ppdu_id are
			 * different there is a payload already.
			 */
			if (qdf_unlikely(ppdu_desc->user[0].completion_status ==
					 HTT_PPDU_STATS_USER_STATUS_OK)) {
				if (mem_limit_flag) {
					uint32_t nbytes = get_queue_bytes(retries_q);
					qdf_atomic_sub(nbytes,
						       &mon_soc->dp_soc_tx_capt.ppdu_mgmt_bytes);
				}
				TX_CAP_NBUF_QUEUE_FREE(retries_q);
			}

			status = QDF_STATUS_SUCCESS;

insert_mgmt_buf_to_queue:

			if (mem_limit_flag) {
				if (!dp_tx_capt_mem_check(pdev,
						qdf_nbuf_get_truesize(mgmt_ctl_nbuf))) {
					mon_soc->dp_soc_tx_capt.mem_limit_drops++;
					qdf_nbuf_free(mgmt_ctl_nbuf);
					status =  QDF_STATUS_E_FAILURE;
					goto exit;
				} else {
					qdf_atomic_add(qdf_nbuf_get_truesize(mgmt_ctl_nbuf),
							&mon_soc->dp_soc_tx_capt.ppdu_mgmt_bytes);
				}
			}

			/*
			 * insert the mgmt_ctl buffer back to
			 * the queue
			 */
			qdf_spin_lock_bh(
			&mon_pdev->tx_capture.ctl_mgmt_lock[type][subtype]);
			qdf_nbuf_queue_insert_head(
			&mon_pdev->tx_capture.ctl_mgmt_q[type][subtype],
			mgmt_ctl_nbuf);
			qdf_spin_unlock_bh(
			&mon_pdev->tx_capture.ctl_mgmt_lock[type][subtype]);
		} else {
			qdf_nbuf_t nbuf_retry_ppdu;
			struct cdp_tx_completion_ppdu *tmp_ppdu_desc;
			uint16_t frame_ctrl_le;
			struct ieee80211_frame *wh;
			uint32_t retry_len = 0;

			dp_tx_capture_info("%pK: ppdu_id[m:%d desc:%d] start_tsf: %llu mgmt_tsf:%llu bar_frm_with_data:%d is_sgen:%d",
					   pdev->soc, ppdu_id, desc_ppdu_id,
					   start_tsf, ptr_comp_info->tx_tsf,
					   bar_frm_with_data, is_sgen_pkt);

			/* pull head based on sgen pkt or mgmt pkt */
			if (NULL == qdf_nbuf_pull_head(mgmt_ctl_nbuf,
						       head_size)) {
				dp_tx_capture_alert("%pK: No Head space to pull !!\n", pdev->soc);
				qdf_assert_always(0);
			}

			wh = (struct ieee80211_frame *)
				(qdf_nbuf_data(mgmt_ctl_nbuf));

			if (type == IEEE80211_FC0_TYPE_MGT &&
			    (subtype == MGMT_SUBTYPE_PROBE_RESP >> 4 ||
			     subtype == MGMT_SUBTYPE_DISASSOC >> 4 ||
			     subtype == MGMT_SUBTYPE_DEAUTH >> 4 ||
			     subtype == MGMT_SUBTYPE_AUTH >> 4)) {
				if (!dp_peer_or_pdev_tx_cap_enabled(pdev,
								    NULL,
								    wh->i_addr1
								    )) {
					qdf_nbuf_free(nbuf_ppdu_desc);
					qdf_nbuf_free(mgmt_ctl_nbuf);

					if (mem_limit_flag) {
						uint32_t nbytes = get_queue_bytes(retries_q);
						qdf_atomic_sub(nbytes,
							       &mon_soc->dp_soc_tx_capt.ppdu_mgmt_bytes);
					}
					TX_CAP_NBUF_QUEUE_FREE(retries_q);
					status = QDF_STATUS_SUCCESS;
					goto exit;
				}
			}

			while (qdf_nbuf_queue_len(retries_q)) {
				/*
				 * send retried packet stored
				 * in queue
				 */
				nbuf_retry_ppdu =
					qdf_nbuf_queue_remove(retries_q);

				retry_len = qdf_nbuf_queue_len(retries_q);
				if (!nbuf_retry_ppdu) {
					dp_tx_capture_alert("%pK: retry q type[%d][%d] retry q len = %d\n",
							    pdev->soc,
							    type,
							    subtype,
							    retry_len);
					qdf_assert_always(0);
					break;
				}

				if (wlan_cfg_get_tx_capt_max_mem(pdev->soc->wlan_cfg_ctx)) {
					qdf_atomic_sub(qdf_nbuf_get_truesize(nbuf_retry_ppdu),
							&mon_soc->dp_soc_tx_capt.ppdu_mgmt_bytes);
				}

				tmp_ppdu_desc =
					(struct cdp_tx_completion_ppdu *)
						qdf_nbuf_data(nbuf_retry_ppdu);
				tmp_mgmt_ctl_nbuf =
					qdf_nbuf_copy_expand(mgmt_ctl_nbuf,
							     0, 0);
				if (qdf_unlikely(!tmp_mgmt_ctl_nbuf)) {
					dp_tx_capture_alert("%pK: No memory to do copy!!", pdev->soc);
					qdf_assert_always(0);
				}

				dp_update_tx_cap_info(pdev, nbuf_retry_ppdu,
						      &tx_capture_info, true,
						      bar_frm_with_data);
				if (!tx_capture_info.mpdu_nbuf) {
					qdf_nbuf_free(nbuf_retry_ppdu);
					qdf_nbuf_free(tmp_mgmt_ctl_nbuf);
					continue;
				}

				/*
				 * frame control from ppdu_desc has
				 * retry flag set
				 */
				frame_ctrl_le =
				qdf_cpu_to_le16(tmp_ppdu_desc->frame_ctrl);
				wh = (struct ieee80211_frame *)
					(qdf_nbuf_data(tmp_mgmt_ctl_nbuf));
				wh->i_fc[1] = (frame_ctrl_le & 0xFF00) >> 8;
				wh->i_fc[0] = (frame_ctrl_le & 0xFF);

				tx_capture_info.ppdu_desc = tmp_ppdu_desc;
				/*
				 * send MPDU to osif layer
				 */
				status = dp_send_mgmt_ctrl_to_stack(
							pdev,
							nbuf_retry_ppdu,
							&tx_capture_info,
							tmp_mgmt_ctl_nbuf,
							true);
			}

			dp_update_tx_cap_info(pdev, nbuf_ppdu_desc,
					      &tx_capture_info, true,
					      bar_frm_with_data);
			if (!tx_capture_info.mpdu_nbuf) {
				qdf_nbuf_free(mgmt_ctl_nbuf);
				qdf_nbuf_free(nbuf_ppdu_desc);
				status = QDF_STATUS_SUCCESS;
				goto exit;
			}

			tx_capture_info.mpdu_info.ppdu_id =
				*(uint32_t *)qdf_nbuf_data(mgmt_ctl_nbuf);

			/* frame control from ppdu_desc has retry flag set */
			frame_ctrl_le = qdf_cpu_to_le16(ppdu_desc_frame_ctrl);
			wh = (struct ieee80211_frame *)
				(qdf_nbuf_data(mgmt_ctl_nbuf));
			wh->i_fc[1] = (frame_ctrl_le & 0xFF00) >> 8;
			wh->i_fc[0] = (frame_ctrl_le & 0xFF);

			tx_capture_info.ppdu_desc = ppdu_desc;
			/*
			 * send MPDU to osif layer
			 */
			status = dp_send_mgmt_ctrl_to_stack(pdev,
							    nbuf_ppdu_desc,
							    &tx_capture_info,
							    mgmt_ctl_nbuf,
							    true);
		}
	} else if (!is_sgen_pkt) {
		/*
		 * only for the packets send over the air are handled
		 * packets drop by firmware is not handled in this
		 * feature
		 */
		if (user->completion_status ==
		    HTT_PPDU_STATS_USER_STATUS_FILTERED) {
			qdf_nbuf_free(nbuf_ppdu_desc);
			status = QDF_STATUS_SUCCESS;
			goto exit;
		}

		/* check for max retry count */
		if (qdf_nbuf_queue_len(retries_q) >= MAX_RETRY_Q_COUNT) {
			qdf_nbuf_t nbuf_retry_ppdu;

			nbuf_retry_ppdu =
				qdf_nbuf_queue_remove(retries_q);

			if (mem_limit_flag) {
				if (qdf_likely(nbuf_retry_ppdu)) {
					qdf_atomic_sub(qdf_nbuf_get_truesize(nbuf_retry_ppdu),
						&mon_soc->dp_soc_tx_capt.ppdu_mgmt_bytes);
				}
			}
			qdf_nbuf_free(nbuf_retry_ppdu);
		}

		/* if adding the new buffer goes beyond the allowed limit,
		 * drop the buffer
		 */
		if (mem_limit_flag) {
			if (!dp_tx_capt_mem_check(pdev,
							qdf_nbuf_get_truesize(nbuf_ppdu_desc))) {
				mon_soc->dp_soc_tx_capt.mem_limit_drops++;
				qdf_nbuf_free(nbuf_ppdu_desc);
				status = QDF_STATUS_SUCCESS;
				goto exit;
			} else {
				qdf_atomic_add(qdf_nbuf_get_truesize(nbuf_ppdu_desc),
							&mon_soc->dp_soc_tx_capt.ppdu_mgmt_bytes);
			}
		}

		/*
		 * add the ppdu_desc into retry queue
		 */
		qdf_nbuf_queue_add(retries_q, nbuf_ppdu_desc);

		/* flushing retry queue since completion status is
		 * in final state. meaning that even though ppdu_id are
		 * different there is a payload already.
		 */
		if (qdf_unlikely(ppdu_desc->user[0].completion_status ==
				 HTT_PPDU_STATS_USER_STATUS_OK)) {
			if (mem_limit_flag) {
				uint32_t nbytes = get_queue_bytes(retries_q);
				qdf_atomic_sub(nbytes,
					       &mon_soc->dp_soc_tx_capt.ppdu_mgmt_bytes);
			}
			TX_CAP_NBUF_QUEUE_FREE(retries_q);
		}

		status = QDF_STATUS_SUCCESS;
	} else if ((ppdu_desc_frame_ctrl &
		   IEEE80211_FC0_TYPE_MASK) ==
		   IEEE80211_FC0_TYPE_CTL) {
		dp_update_tx_cap_info(pdev, nbuf_ppdu_desc,
				      &tx_capture_info, false,
				      bar_frm_with_data);
		if (!tx_capture_info.mpdu_nbuf) {
			qdf_nbuf_free(nbuf_ppdu_desc);
			status = QDF_STATUS_SUCCESS;
			goto exit;
		}
		/*
		 * send MPDU to osif layer
		 */
		status = dp_send_mgmt_ctrl_to_stack(pdev, nbuf_ppdu_desc,
						    &tx_capture_info,
						    NULL, false);
	}

exit:
	return status;
}

/**
 * dp_peer_tx_cap_tid_queue_flush_tlv(): Function to dequeue peer queue
 * @pdev: DP pdev handle
 * @peer; DP peer handle
 * @ppdu_desc: ppdu_desc
 *
 * return: void
 */
static void
dp_peer_tx_cap_tid_queue_flush_tlv(struct dp_pdev *pdev,
				   struct dp_peer *peer,
				   struct cdp_tx_completion_ppdu *ppdu_desc,
				   uint8_t usr_idx)
{
	int tid;
	struct dp_tx_tid *tx_tid;
	qdf_nbuf_queue_t head_xretries;
	qdf_nbuf_queue_t head_msdu;
	uint32_t qlen = 0;
	uint32_t qlen_curr = 0;
	struct cdp_tx_completion_ppdu_user *user;
	struct dp_mon_peer *mon_peer = peer->monitor_peer;

	user = &ppdu_desc->user[usr_idx];
	tid = user->tid;
	tx_tid = &mon_peer->tx_capture.tx_tid[tid];

	qdf_nbuf_queue_init(&head_msdu);
	qdf_nbuf_queue_init(&head_xretries);

	qlen = qdf_nbuf_queue_len(&tx_tid->defer_msdu_q);

	dp_tx_msdu_dequeue(peer, INVALID_PPDU_ID,
			   tid, ppdu_desc->num_msdu,
			   &head_msdu,
			   &head_xretries,
			   0, MAX_END_TSF);

	dp_tx_cap_stats_msdu_update(peer, PEER_MSDU_FLUSH,
				    qdf_nbuf_queue_len(&head_msdu));
	dp_tx_cap_stats_msdu_update(peer, PEER_MSDU_FLUSH,
				    qdf_nbuf_queue_len(&head_xretries));
	if (!qdf_nbuf_is_queue_empty(&head_xretries)) {
		struct cdp_tx_completion_ppdu *xretry_ppdu = NULL;
		struct cdp_tx_completion_ppdu_user *xretry_user = NULL;
		uint32_t xretry_qlen;

		xretry_ppdu = tx_tid->xretry_ppdu;
		if (!xretry_ppdu) {
			dp_tx_capture_alert("%pK: xretry_ppdu is NULL",
					    pdev->soc);
			return;
		}

		xretry_user = &xretry_ppdu->user[0];
		if (!xretry_user) {
			dp_tx_capture_alert("%pK: xretry_user is NULL",
					    pdev->soc);
			return;
		}

		xretry_ppdu->ppdu_id = mon_peer->tx_capture.tx_wifi_ppdu_id;

		/* Restitch MPDUs from xretry MSDUs */
		dp_tx_mon_restitch_mpdu(pdev, peer,
					xretry_ppdu,
					&head_xretries,
					&xretry_user->mpdu_q,
					0);

		xretry_qlen = qdf_nbuf_queue_len(&xretry_user->mpdu_q);
		dp_tx_cap_stats_mpdu_update(peer, PEER_MPDU_RESTITCH,
					    xretry_qlen);
	}
	TX_CAP_NBUF_QUEUE_FREE(&head_msdu);
	TX_CAP_NBUF_QUEUE_FREE(&head_xretries);
	qlen_curr = qdf_nbuf_queue_len(&tx_tid->defer_msdu_q);

	dp_tx_mon_proc_xretries(pdev, peer, tid);

	dp_tx_capture_info("%pK: peer_id [%d %pK] tid[%d] qlen[%d -> %d]",
			   pdev->soc, ppdu_desc->user[usr_idx].peer_id,
			   peer, tid, qlen, qlen_curr);
}

/**
 * dp_tx_ppdu_stats_flush(): Function to flush pending retried ppdu desc
 * @pdev: DP pdev handle
 * @nbuf: ppdu_desc
 *
 * return: void
 */
static void
dp_tx_ppdu_stats_flush(struct dp_pdev *pdev,
		       struct cdp_tx_completion_ppdu *ppdu_desc,
			   uint8_t usr_idx)
{
	struct dp_peer *peer;
	struct cdp_tx_completion_ppdu_user *user;

	user = &ppdu_desc->user[usr_idx];
	peer = DP_TX_PEER_GET_REF(pdev, user->peer_id);

	if (!peer)
		return;

	if (peer->monitor_peer->tx_capture.is_tid_initialized)
		dp_peer_tx_cap_tid_queue_flush_tlv(pdev, peer,
						   ppdu_desc, usr_idx);

	DP_TX_PEER_DEL_REF(peer);
}

/**
 * dp_check_ppdu_and_deliver(): Check PPDUs for any holes and deliver
 * to upper layer if complete
 * @pdev: DP pdev handle
 * @nbuf_ppdu_list: ppdu_desc_list per sched cmd id
 * @ppdu_desc_cnt: number of ppdu_desc_cnt
 *
 * return: status
 */
static void
dp_check_ppdu_and_deliver(struct dp_pdev *pdev,
			  struct dp_tx_cap_nbuf_list nbuf_ppdu_list[],
			  uint32_t ppdu_desc_cnt)
{
	uint32_t ppdu_id;
	uint32_t desc_cnt;
	qdf_nbuf_t tmp_nbuf;
	struct dp_tx_tid *tx_tid  = NULL;
	int i;
	uint8_t max_num_users = 0;
	uint8_t usr_idx;
	struct dp_tx_cap_nbuf_list *ptr_nbuf_list;
	struct dp_mon_pdev *mon_pdev = pdev->monitor_pdev;

	for (desc_cnt = 0; desc_cnt < ppdu_desc_cnt; desc_cnt++) {
		struct cdp_tx_completion_ppdu *ppdu_desc;
		struct cdp_tx_completion_ppdu_user *user;
		uint32_t num_mpdu;
		uint16_t start_seq, seq_no = 0;
		int i;
		qdf_nbuf_t mpdu_nbuf;
		struct dp_peer *peer;
		uint8_t type;
		uint8_t subtype;
		uint8_t usr_type;
		uint32_t mpdus_tried;
		uint8_t num_users;
		qdf_nbuf_t nbuf_ppdu;
		bool is_bar_frm_with_data = false;
		struct dp_mon_peer *mon_peer;

		ptr_nbuf_list = &nbuf_ppdu_list[desc_cnt];

		if (!dp_tx_cap_nbuf_list_get_ref(ptr_nbuf_list)) {
			if (ptr_nbuf_list->nbuf_ppdu)
				qdf_assert_always(0);
			continue;
		}

		nbuf_ppdu = ptr_nbuf_list->nbuf_ppdu;
		ppdu_desc = (struct cdp_tx_completion_ppdu *)
			qdf_nbuf_data(nbuf_ppdu);

		ppdu_id = ppdu_desc->ppdu_id;
		num_users = ppdu_desc->num_users;

		if (ppdu_desc->is_flush) {
			dp_tx_ppdu_stats_flush(pdev, ppdu_desc, 0);
			dp_ppdu_desc_free_all(pdev, ptr_nbuf_list, num_users);
			continue;
		}

		if (max_num_users < ppdu_desc->num_users)
			max_num_users = ppdu_desc->num_users;

		type = (ppdu_desc->frame_ctrl & IEEE80211_FC0_TYPE_MASK);
		subtype = (ppdu_desc->frame_ctrl &
			   IEEE80211_FC0_SUBTYPE_MASK);
		usr_type = (ppdu_desc->user[0].frame_ctrl &
			    IEEE80211_FC0_TYPE_MASK);

		/* handle management frame */
		if ((type != IEEE80211_FC0_TYPE_DATA) ||
		    (ppdu_desc->htt_frame_type ==
		     HTT_STATS_FTYPE_SGEN_MU_BAR) ||
		    (ppdu_desc->htt_frame_type ==
		     HTT_STATS_FTYPE_SGEN_QOS_NULL)) {
			qdf_nbuf_t tmp_nbuf_ppdu;
			QDF_STATUS status;

			tmp_nbuf_ppdu = nbuf_ppdu;
			/*
			 * take reference of ppdu_desc if the htt_frame_type is
			 * HTT_STATS_FTYPE_SGEN_MU_BAR, as there will be
			 * corresponding data frame
			 */
			if (((type == IEEE80211_FC0_TYPE_CTL) &&
			     (subtype == IEEE80211_FC0_SUBTYPE_BAR) &&
			     (usr_type == IEEE80211_FC0_TYPE_DATA)) ||
			    (ppdu_desc->htt_frame_type ==
			     HTT_STATS_FTYPE_SGEN_MU_BAR)) {
				/*
				 * clonning ppdu_desc additional reference as
				 * handling data frame
				 */
				tmp_nbuf_ppdu = qdf_nbuf_clone(nbuf_ppdu);
				if (qdf_unlikely(!tmp_nbuf_ppdu)) {
					qdf_assert_always(0);
					continue;
				}

				dp_tx_cap_nbuf_list_inc_ref(ptr_nbuf_list);
				is_bar_frm_with_data = true;
			}

			status = dp_check_mgmt_ctrl_ppdu(pdev, tmp_nbuf_ppdu,
							 is_bar_frm_with_data);

			dp_tx_cap_nbuf_list_dec_ref(ptr_nbuf_list);
			if (status == QDF_STATUS_E_FAILURE) {
				/*
				 * QDF_STATUS_E_FAILURE - when tmp_nbuf_ppdu
				 * is not free in dp_check_mgmt_ctrl_ppdu()
				 */
				qdf_nbuf_free(tmp_nbuf_ppdu);
			}

			if (!is_bar_frm_with_data)
				continue;
		}

		/*
		 * process only data frame and other
		 */
		for (usr_idx = 0; usr_idx < num_users; usr_idx++) {
			uint32_t mpdu_enq = 0;
			uint32_t mpdu_tried = 0;

			if (!ptr_nbuf_list->nbuf_ppdu ||
			    !dp_tx_cap_nbuf_list_get_ref(ptr_nbuf_list)) {
				continue;
			}

			nbuf_ppdu = ptr_nbuf_list->nbuf_ppdu;

			ppdu_desc = (struct cdp_tx_completion_ppdu *)
					qdf_nbuf_data(nbuf_ppdu);

			user = &ppdu_desc->user[usr_idx];

			if (user->delayed_ba || user->skip == 1)
				continue;

			peer = DP_TX_PEER_GET_REF(pdev, user->peer_id);
			if (!peer) {
				user->skip = 1;
				dp_ppdu_desc_free(pdev, ptr_nbuf_list, usr_idx);
				continue;
			}

			mon_peer = peer->monitor_peer;

			if (!mon_peer->tx_capture.is_tid_initialized) {
				user->skip = 1;
				dp_ppdu_desc_free(pdev, ptr_nbuf_list, usr_idx);
				DP_TX_PEER_DEL_REF(peer);
				continue;
			}

			tx_tid = &mon_peer->tx_capture.tx_tid[user->tid];
			tx_tid->last_processed_ms =
				qdf_system_ticks_to_msecs(qdf_system_ticks());
			ppdu_id = ppdu_desc->ppdu_id;

			/* find mpdu tried is same as success mpdu */
			num_mpdu = user->mpdu_success;

			/*
			 * ba_size is updated in BA bitmap TLVs,
			 * which are not received
			 * in case of non-QoS TID.
			 */
			if (qdf_unlikely(user->tid == DP_NON_QOS_TID)) {
				user->ba_size = 1;
				user->last_enq_seq = user->start_seq;
			}

			if (user->ba_size == 0)
				user->ba_size = 1;

			/* find list of missing sequence */
			user->mpdus = qdf_mem_malloc(sizeof(qdf_nbuf_t) *
						     user->ba_size);

			if (qdf_unlikely(!user->mpdus)) {
				dp_tx_capture_alert("%pK: ppdu_desc->mpdus allocation failed",
						    pdev->soc);
				dp_ppdu_desc_free_all(pdev, ptr_nbuf_list, num_users);
				DP_TX_PEER_DEL_REF(peer);
				dp_print_pdev_tx_capture_stats_1_0(pdev);
				qdf_assert_always(0);
				return;
			}

			if (qdf_unlikely(user->ba_size >
			    CDP_BA_256_BIT_MAP_SIZE_DWORDS *
				SEQ_SEG_SZ_BITS(user->failed_bitmap))) {
				DP_TX_PEER_DEL_REF(peer);
				qdf_assert_always(0);
				return;
			}
			/* Fill seq holes within current schedule list */
			start_seq = user->start_seq;
			seq_no = 0;
			mpdus_tried = user->mpdu_tried_mcast +
				user->mpdu_tried_ucast;

			for (i = 0; (i < user->ba_size) && mpdus_tried; i++) {
				if (qdf_likely(user->tid != DP_NON_QOS_TID) &&
				    !(SEQ_BIT(user->enq_bitmap, i)) &&
				    !mpdu_tried)
					continue;
				mpdus_tried--;
				/* missed seq number */
				seq_no = start_seq + i;

				/*
				 * Fill failed MPDUs in AMPDU if they're
				 * available in subsequent PPDUs in current
				 * burst schedule. This is not applicable
				 * for non-QoS TIDs (no AMPDUs)
				 */
				if (qdf_likely(user->tid != DP_NON_QOS_TID) &&
				    !(SEQ_BIT(user->failed_bitmap, i))) {
					uint8_t seq_idx;

					dp_tx_capture_debug("%pK:find seq %d in next ppdu %d",
							    pdev->soc, seq_no,
							    ppdu_desc_cnt);

					mpdu_nbuf =
						get_mpdu_clone_from_next_ppdu(
							nbuf_ppdu_list +
							desc_cnt,
							ppdu_desc_cnt -
							desc_cnt, seq_no,
							user->peer_id,
							ppdu_id, usr_idx);

					seq_idx = seq_no - start_seq;
					/* check mpdu_nbuf NULL */
					if (!mpdu_nbuf) {
						qdf_nbuf_t m_nbuf = NULL;

						m_nbuf = user->mpdus[seq_idx];
						CHECK_MPDUS_NULL(m_nbuf);
						user->mpdus[seq_idx] = NULL;
						user->pending_retries++;
						continue;
					}

					dp_tx_cap_stats_mpdu_update(peer,
							PEER_MPDU_CLONE, 1);

					CHECK_MPDUS_NULL(user->mpdus[seq_idx]);
					user->mpdus[seq_idx] = mpdu_nbuf;

					SEQ_SEG(user->failed_bitmap, i) |=
					SEQ_SEG_MSK(user->failed_bitmap[0], i);
				} else {
					qdf_nbuf_queue_t *tmp_q;
					uint32_t index = 0;

					tmp_q = &user->mpdu_q;
					/* any error case we need to handle */
					mpdu_nbuf =
						qdf_nbuf_queue_remove(tmp_q);
					/* check mpdu_nbuf NULL */
					if (!mpdu_nbuf)
						continue;

					index = seq_no - start_seq;
					CHECK_MPDUS_NULL(user->mpdus[index]);
					user->mpdus[index] = mpdu_nbuf;
					dp_tx_cap_stats_mpdu_update(peer,
							PEER_MPDU_ARR, 1);
				}
			}

			for (/* get i from previous stored value*/;
			     i < user->ba_size; i++) {
				qdf_nbuf_queue_t *tmp_q;

				/* missed seq number */
				seq_no = start_seq + i;

				tmp_q = &user->mpdu_q;
				/* any error case we need to handle */
				mpdu_nbuf = qdf_nbuf_queue_remove(tmp_q);
				/* check mpdu_nbuf NULL */
				if (!mpdu_nbuf)
					continue;

				user->mpdus[seq_no - start_seq] = mpdu_nbuf;
				dp_tx_cap_stats_mpdu_update(peer,
							    PEER_MPDU_ARR, 1);

				SEQ_SEG(user->failed_bitmap, i) |=
					SEQ_SEG_MSK(user->failed_bitmap[0], i);
			}

			mpdu_tried = user->mpdu_tried_ucast +
					user->mpdu_tried_mcast;
			for (i = 0; i < CDP_BA_256_BIT_MAP_SIZE_DWORDS; i++)
				mpdu_enq +=
					get_number_of_1s(user->enq_bitmap[i]);

			if (mpdu_tried > mpdu_enq)
				dp_ppdu_desc_debug_print(ppdu_desc, usr_idx,
							 __func__, __LINE__);

			/*
			 * It is possible that enq_bitmap received has
			 * more bits than actual mpdus tried if HW was
			 * unable to send all MPDUs, and last_enq_seq
			 * and ba_size should be adjusted in that case
			 */
			if (i < user->ba_size) {
				user->last_enq_seq = seq_no;
				user->ba_size = seq_no - start_seq + 1;
			}

			DP_TX_PEER_DEL_REF(peer);
		}
	}

	for (usr_idx = 0; usr_idx < max_num_users; usr_idx++) {
		for (i = 0; i < ppdu_desc_cnt; i++) {
			uint32_t pending_ppdus;
			struct cdp_tx_completion_ppdu *cur_ppdu_desc;
			struct cdp_tx_completion_ppdu_user *cur_user;
			struct dp_peer *peer;
			qdf_nbuf_queue_t head_ppdu;
			uint16_t peer_id;
			struct dp_mon_peer *mon_peer;

			ptr_nbuf_list = &nbuf_ppdu_list[i];

			if (!ptr_nbuf_list->nbuf_ppdu ||
			    !dp_tx_cap_nbuf_list_get_ref(ptr_nbuf_list))
				continue;

			cur_ppdu_desc = (struct cdp_tx_completion_ppdu *)
					qdf_nbuf_data(ptr_nbuf_list->nbuf_ppdu);

			if (!cur_ppdu_desc)
				continue;

			if (usr_idx >= cur_ppdu_desc->num_users)
				continue;

			cur_user = &cur_ppdu_desc->user[usr_idx];

			if ((cur_user->delayed_ba == 1) ||
			    (cur_user->skip == 1) || (cur_user->mon_procd == 1))
				continue;

			peer_id = cur_ppdu_desc->user[usr_idx].peer_id;
			peer = DP_TX_PEER_GET_REF(pdev, peer_id);
			if (!peer) {
				dp_ppdu_desc_free(pdev, ptr_nbuf_list, usr_idx);
				continue;
			}

			mon_peer = peer->monitor_peer;
			if (!mon_peer->tx_capture.is_tid_initialized) {
				dp_ppdu_desc_free(pdev, ptr_nbuf_list, usr_idx);
				DP_TX_PEER_DEL_REF(peer);
				continue;
			}

			tx_tid = &mon_peer->tx_capture.tx_tid[cur_user->tid];
			qdf_nbuf_queue_init(&head_ppdu);
			dp_tx_mon_proc_pending_ppdus(pdev, tx_tid,
						     nbuf_ppdu_list + i,
						     ppdu_desc_cnt - i,
						     &head_ppdu,
						     cur_user->peer_id,
						     usr_idx);

			if (qdf_nbuf_is_queue_empty(&tx_tid->pending_ppdu_q)) {
				while ((tmp_nbuf =
					qdf_nbuf_queue_first(&head_ppdu))) {
					cur_ppdu_desc =
					(struct cdp_tx_completion_ppdu *)
						qdf_nbuf_data(tmp_nbuf);
					cur_user =
						&cur_ppdu_desc->user[usr_idx];

					if (cur_user->pending_retries)
						break;
					dp_send_data_to_stack(pdev,
							      cur_ppdu_desc,
							      usr_idx);
					dp_ppdu_queue_free(pdev, tmp_nbuf, usr_idx);
					qdf_nbuf_queue_remove(&head_ppdu);
					qdf_nbuf_free(tmp_nbuf);
				}
			}
			qdf_nbuf_queue_append(&tx_tid->pending_ppdu_q,
					      &head_ppdu);

			dp_tx_mon_proc_xretries(pdev, peer, tx_tid->tid);

			pending_ppdus =
				qdf_nbuf_queue_len(&tx_tid->pending_ppdu_q);
			if ((pending_ppdus > MAX_PENDING_PPDUS) ||
			    (pending_ppdus &&
			     wlan_cfg_get_tx_capt_max_mem(pdev->soc->wlan_cfg_ctx) &&
			     !dp_tx_capt_mem_check(pdev, 0))) {
				struct cdp_tx_completion_ppdu *tmp_ppdu_desc;
				uint8_t tmp_usr_idx;
				qdf_nbuf_queue_t *tmp_ppdu_q;

				dp_tx_capture_err("%pK: pending ppdus (%d, %d) : %d\n",
						  pdev->soc, peer_id,
						  tx_tid->tid, pending_ppdus);
				tmp_ppdu_q = &tx_tid->pending_ppdu_q;
				tmp_nbuf = qdf_nbuf_queue_remove(tmp_ppdu_q);
				if (qdf_unlikely(!tmp_nbuf)) {
					DP_TX_PEER_DEL_REF(peer);
					qdf_assert_always(0);
					return;
				}

				tmp_ppdu_desc =
					(struct cdp_tx_completion_ppdu *)
					qdf_nbuf_data(tmp_nbuf);
				tmp_usr_idx = dp_tx_find_usr_idx_from_peer_id(
						tmp_ppdu_desc, peer_id);
				dp_send_data_to_stack(pdev, tmp_ppdu_desc,
						      tmp_usr_idx);
				dp_ppdu_queue_free(pdev, tmp_nbuf, tmp_usr_idx);
				qdf_nbuf_free(tmp_nbuf);
				mon_pdev->tx_capture.pend_ppdu_dropped++;
			}
			DP_TX_PEER_DEL_REF(peer);
		}
	}
}

static uint32_t
dp_tx_cap_proc_per_ppdu_info(struct dp_pdev *pdev, qdf_nbuf_t nbuf_ppdu,
			     struct dp_tx_cap_nbuf_list nbuf_ppdu_list[],
			     uint32_t ppdu_desc_cnt)
{
	struct dp_tx_cap_nbuf_list *ptr_nbuf_list;
	struct cdp_tx_completion_ppdu *ppdu_desc = NULL;
	struct dp_peer *peer = NULL;
	qdf_nbuf_queue_t head_msdu;
	qdf_nbuf_queue_t head_xretries;
	uint32_t retries = 0;
	uint32_t ret = 0;
	uint32_t start_tsf = 0;
	uint32_t end_tsf = 0;
	uint32_t bar_start_tsf = 0;
	uint32_t bar_end_tsf = 0;
	uint16_t tid = 0;
	uint32_t num_msdu = 0;
	uint32_t qlen = 0;
	uint16_t peer_id;
	uint8_t type, subtype;
	uint8_t usr_idx;
	bool is_bar_frm_with_data = false;
	uint8_t usr_type;
	uint8_t usr_subtype;

	qdf_nbuf_queue_init(&head_msdu);
	qdf_nbuf_queue_init(&head_xretries);

	ppdu_desc = (struct cdp_tx_completion_ppdu *)qdf_nbuf_data(nbuf_ppdu);
	type = (ppdu_desc->frame_ctrl &
		IEEE80211_FC0_TYPE_MASK);
	subtype = (ppdu_desc->frame_ctrl &
		   IEEE80211_FC0_SUBTYPE_MASK);

	if ((type == IEEE80211_FC0_TYPE_DATA) &&
	    (subtype == IEEE80211_FC0_SUBTYPE_QOS_NULL) &&
	    (ppdu_desc->htt_frame_type ==
	     HTT_STATS_FTYPE_TIDQ_DATA_SU)) {
		ppdu_desc->htt_frame_type =
			HTT_STATS_FTYPE_SGEN_QOS_NULL;
	}

	usr_type = (ppdu_desc->user[0].frame_ctrl &
		    IEEE80211_FC0_TYPE_MASK);
	usr_subtype = (ppdu_desc->user[0].frame_ctrl &
		       IEEE80211_FC0_SUBTYPE_MASK);

	if (((type == IEEE80211_FC0_TYPE_CTL) &&
	     (subtype == IEEE80211_FC0_SUBTYPE_BAR) &&
	     (usr_type == IEEE80211_FC0_TYPE_DATA)) ||
	    ppdu_desc->htt_frame_type == HTT_STATS_FTYPE_SGEN_MU_BAR)
		is_bar_frm_with_data = true;

	ptr_nbuf_list = &nbuf_ppdu_list[ppdu_desc_cnt];

	/* ppdu start timestamp */
	start_tsf = ppdu_desc->ppdu_start_timestamp;
	end_tsf = ppdu_desc->ppdu_end_timestamp;
	bar_start_tsf = ppdu_desc->bar_ppdu_start_timestamp;
	bar_end_tsf = ppdu_desc->bar_ppdu_end_timestamp;

	if (((ppdu_desc->frame_type == CDP_PPDU_FTYPE_DATA) &&
	     (ppdu_desc->htt_frame_type !=
	      HTT_STATS_FTYPE_SGEN_QOS_NULL)) ||
		is_bar_frm_with_data) {
		uint32_t mpdu_suc;
		uint32_t mpdu_tri;
		uint8_t ref_cnt = 0;
		uint8_t num_users = ppdu_desc->num_users;
		struct dp_tx_tid *tx_tid;
		struct cdp_tx_completion_ppdu *xretry_ppdu;
		struct cdp_tx_completion_ppdu_user *xretry_user;
		struct cdp_tx_completion_ppdu_user *user;
		qdf_nbuf_queue_t *mpdu_q;
		qdf_nbuf_queue_t *x_mpdu_q;
		struct dp_mon_peer *mon_peer;
		struct dp_mon_soc *mon_soc = pdev->soc->monitor_soc;

		for (usr_idx = 0; usr_idx < num_users;
		     usr_idx++) {
			uint32_t ppdu_id;

			peer = NULL;
			user = &ppdu_desc->user[usr_idx];
			if (usr_idx + 1 != num_users)
				qdf_nbuf_ref(nbuf_ppdu);

			if (user->delayed_ba == 1) {
				user->skip = 1;
				goto free_nbuf_dec_ref;
			}

			peer_id = user->peer_id;
			peer = DP_TX_PEER_GET_REF(pdev, peer_id);

			/**
			 * peer can be NULL
			 */
			if (!peer ||
			    !(peer->monitor_peer->tx_capture.is_tid_initialized)) {
				user->skip = 1;
				goto free_nbuf_dec_ref;
			}
			mon_peer = peer->monitor_peer;

			/**
			 * check whether it is bss peer,
			 * if bss_peer no need to process
			 * further check whether tx_capture
			 * feature is enabled for this peer
			 * or globally for all peers
			 */
			if (peer->bss_peer ||
			    !dp_peer_or_pdev_tx_cap_enabled(pdev,
				peer, peer->mac_addr.raw) || user->is_mcast) {
				user->skip = 1;
				goto free_nbuf_dec_ref;
			}

			/* update failed bitmap */
			dp_process_ppdu_stats_update_failed_bitmap(
				pdev, user, ppdu_desc->ppdu_id,
				CDP_BA_256_BIT_MAP_SIZE_DWORDS);
			/* print the bit map */
			dp_tx_print_bitmap(pdev, ppdu_desc,
					   usr_idx,
					   ppdu_desc->ppdu_id);
			if (user->tid >= DP_MAX_TIDS) {
				dp_tx_capture_err("%pK: ppdu[%d] peer_id[%d] TID[%d] > NON_QOS_TID!",
						  pdev->soc,
						  ppdu_desc->ppdu_id,
						  user->peer_id,
						  user->tid);

				user->skip = 1;
				goto free_nbuf_dec_ref;
			}

			if (is_bar_frm_with_data)
				ppdu_id = ppdu_desc->bar_ppdu_id;
			else
				ppdu_id = ppdu_desc->ppdu_id;

			tid = user->tid;
			num_msdu = user->num_msdu;

dequeue_msdu_again:
			/*
			 * retrieve msdu buffer based on ppdu_id & tid
			 * based msdu queue and store it in local queue
			 * sometimes, wbm comes later than per ppdu
			 * stats. Assumption: all packets are SU,
			 * and packets comes in order
			 */
			ret = dp_tx_msdu_dequeue(peer,
						 ppdu_id,
						 tid,
						 num_msdu,
						 &head_msdu,
						 &head_xretries,
						 start_tsf,
						 end_tsf);

			if (!ret && (++retries < 2)) {
				/* wait for wbm to complete */
				qdf_mdelay(2);
				goto dequeue_msdu_again;
			}

			/*
			 * restitch mpdu from xretry msdu
			 * xretry msdu queue empty check is
			 * done inside restitch function
			 */
			tx_tid = &mon_peer->tx_capture.tx_tid[tid];
			xretry_ppdu = tx_tid->xretry_ppdu;
			xretry_user = &xretry_ppdu->user[0];
			xretry_ppdu->ppdu_id =
			mon_peer->tx_capture.tx_wifi_ppdu_id;
			x_mpdu_q = &xretry_user->mpdu_q;

			/* Restitch MPDUs from xretry MSDUs */
			dp_tx_mon_restitch_mpdu(pdev, peer,
						xretry_ppdu,
						&head_xretries,
						x_mpdu_q, 0);

			qlen = qdf_nbuf_queue_len(x_mpdu_q);
			dp_tx_cap_stats_mpdu_update(peer, PEER_MPDU_RESTITCH,
						    qlen);

			if (qdf_nbuf_is_queue_empty(
						&head_msdu)) {
				if (user->completion_status == 2)
					goto nbuf_add_ref;
				user->skip = 1;
				goto free_nbuf_dec_ref;
			}

			mpdu_q = &user->mpdu_q;
			/*
			 * now head_msdu hold - msdu list for
			 * that particular ppdu_id, restitch
			 * mpdu from msdu and create a mpdu
			 * queue
			 */
			dp_tx_mon_restitch_mpdu(pdev,
						peer,
						ppdu_desc,
						&head_msdu,
						mpdu_q,
						usr_idx);
			/*
			 * sanity: free local head msdu queue
			 * do we need this ?
			 */
			TX_CAP_NBUF_QUEUE_FREE(&head_msdu);
			qlen = qdf_nbuf_queue_len(mpdu_q);
			if (qlen > 1)
			dp_tx_cap_stats_mpdu_update(peer, PEER_MPDU_RESTITCH,
						    qlen);

			if (!qlen) {
				user->skip = 1;
				goto free_nbuf_dec_ref;
			}

			if (wlan_cfg_get_tx_capt_max_mem(pdev->soc->wlan_cfg_ctx)) {
				ppdu_desc->ppdu_bytes += user->mpdu_bytes;
			}

			mpdu_suc = user->mpdu_success;
			mpdu_tri = user->mpdu_tried_ucast +
				   user->mpdu_tried_mcast;

			/* print ppdu_desc info for debugging purpose */
			dp_tx_capture_info("%pK: ppdu[%d] b_ppdu_id[%d] p_id[%d], tid[%d], n_mpdu[%d %d] n_msdu[%d] retr[%d] qlen[%d] tsf[%u - %u] b_tsf[%u - %u] dur[%u] seq[%d] ppdu_desc_cnt[%d]",
					   pdev->soc,
					   ppdu_desc->ppdu_id,
					   ppdu_desc->bar_ppdu_id,
					   user->peer_id,
					   user->tid,
					   ppdu_desc->num_mpdu,
					   mpdu_suc,
					   ppdu_desc->num_msdu, retries,
					   qlen,
					   start_tsf, end_tsf,
					   bar_start_tsf, bar_end_tsf,
					   ppdu_desc->tx_duration,
					   user->start_seq,
					   ppdu_desc_cnt);

			dp_tx_cap_stats_mpdu_update(peer, PEER_MPDU_SUCC,
						    mpdu_suc);
			dp_tx_cap_stats_mpdu_update(peer, PEER_MPDU_TRI,
						    mpdu_tri);
nbuf_add_ref:
			DP_TX_PEER_DEL_REF(peer);
			/* get reference count */
			ref_cnt = qdf_nbuf_get_users(nbuf_ppdu);
			continue;

free_nbuf_dec_ref:
			/* get reference before free */
			ref_cnt = qdf_nbuf_get_users(nbuf_ppdu);
			qdf_nbuf_free(nbuf_ppdu);
			ref_cnt--;
			if (peer)
				DP_TX_PEER_DEL_REF(peer);
			continue;
		}

		if (ref_cnt == 0)
			return ppdu_desc_cnt;

		ptr_nbuf_list->nbuf_ppdu = nbuf_ppdu;
		dp_tx_cap_nbuf_list_update_ref(ptr_nbuf_list, ref_cnt);
		ppdu_desc_cnt++;

		if (wlan_cfg_get_tx_capt_max_mem(pdev->soc->wlan_cfg_ctx)) {
			qdf_size_t desc_size = sizeof(*ppdu_desc) +
				(ppdu_desc->max_users *	sizeof(struct cdp_tx_completion_ppdu_user));
			ppdu_desc->ppdu_bytes +=  desc_size;
			qdf_atomic_add(ppdu_desc->ppdu_bytes,
					&mon_soc->dp_soc_tx_capt.ppdu_bytes);
			/* store the descriptor size so that global count can be
			 * decremented by (desc_size + last user->mpdu_bytes
			 * in dp_tx_ppdu_queue_free
			 */
			ppdu_desc->ppdu_bytes = desc_size;
		}
	} else {
		/*
		 * other packet frame also added to
		 * descriptor list
		 */
		/* print ppdu_desc info for debugging purpose */
		dp_tx_capture_info("%pK: ppdu[%d], p_id[%d], tid[%d], fctrl[0x%x 0x%x] ftype[%d] h_frm_t[%d] seq[%d] tsf[%llu b %llu] dur[%u]",
				   pdev->soc, ppdu_desc->ppdu_id,
				   ppdu_desc->user[0].peer_id,
				   ppdu_desc->user[0].tid,
				   ppdu_desc->frame_ctrl,
				   ppdu_desc->user[0].frame_ctrl,
				   ppdu_desc->frame_type,
				   ppdu_desc->htt_frame_type,
				   ppdu_desc->user[0].start_seq,
				   ppdu_desc->ppdu_start_timestamp,
				   ppdu_desc->bar_ppdu_start_timestamp,
				   ppdu_desc->tx_duration);

		ptr_nbuf_list->nbuf_ppdu = nbuf_ppdu;
		dp_tx_cap_nbuf_list_update_ref(ptr_nbuf_list,
					       1);
		ppdu_desc_cnt++;
	}

	return ppdu_desc_cnt;
}

/*
 * dp_pdev_tx_cap_flush() - flush pdev queue and peer queue
 * @pdev: Datapath pdev
 * @is_stats_queue_empty: stats queue status
 *
 * return: void
 */
static void
dp_pdev_tx_cap_flush(struct dp_pdev *pdev, bool is_stats_queue_empty)
{
	struct dp_mon_pdev *mon_pdev = pdev->monitor_pdev;
	struct dp_pdev_tx_capture *ptr_tx_cap = &mon_pdev->tx_capture;
	struct dp_pdev_flush flush = {0};
	uint32_t now_ms = 0;
	uint32_t delta_ms = 0;
	uint32_t i = 0, j = 0;
	bool mem_limit_flag = false;
	struct dp_mon_soc *mon_soc = pdev->soc->monitor_soc;

	mem_limit_flag = wlan_cfg_get_tx_capt_max_mem(pdev->soc->wlan_cfg_ctx);

	now_ms = qdf_system_ticks_to_msecs(qdf_system_ticks());
	delta_ms = now_ms - ptr_tx_cap->last_processed_ms;

	/*
	 * check difference is more than 3 sec and stats queue is empty
	 * then invoke flush all else, check if difference is more
	 * than 1.5 second invoke peer check to free unflushed queue
	 */
	if (delta_ms > TX_CAPTURE_NO_PPDU_DESC_MS && is_stats_queue_empty)
		flush.flush_all = true;
	else if (delta_ms > TX_CAPTURE_PEER_CHECK_MS && !is_stats_queue_empty)
		flush.flush_all = false;
	else
		return;

	/* update last process ppdu stats queue with current msec */
	ptr_tx_cap->last_processed_ms = now_ms;
	flush.now_ms = now_ms;

	/*
	 * iterate over peer and flush the unreleased queue
	 * which is in queue without processing.
	 */
	dp_pdev_iterate_peer(pdev, dp_peer_tx_cap_tid_queue_flush,
			     &flush, DP_MOD_ID_TX_CAPTURE);

	/* free mgmt packet */
	for (i = 0; i < TXCAP_MAX_TYPE; i++) {
		for (j = 0; j < TXCAP_MAX_SUBTYPE; j++) {
			qdf_nbuf_queue_t *retries_q;

			if (mem_limit_flag) {
				uint32_t nbytes = get_queue_bytes(&mon_pdev->tx_capture.ctl_mgmt_q[i][j]);
				qdf_atomic_sub(nbytes,
					       &mon_soc->dp_soc_tx_capt.ppdu_mgmt_bytes);
			}

			qdf_spin_lock_bh(
				&mon_pdev->tx_capture.ctl_mgmt_lock[i][j]);
			TX_CAP_NBUF_QUEUE_FREE(
				&mon_pdev->tx_capture.ctl_mgmt_q[i][j]);
			qdf_spin_unlock_bh(
				&mon_pdev->tx_capture.ctl_mgmt_lock[i][j]);
			/*
			 * no lock required for retries ctrl mgmt queue
			 * as it is used only in workqueue function.
			 */
			retries_q =
				&mon_pdev->tx_capture.retries_ctl_mgmt_q[i][j];
			if (!qdf_nbuf_is_queue_empty(retries_q)) {
				if (mem_limit_flag) {
					uint32_t nbytes = get_queue_bytes(retries_q);
					qdf_atomic_sub(nbytes,
						       &mon_soc->dp_soc_tx_capt.ppdu_mgmt_bytes);
				}
				TX_CAP_NBUF_QUEUE_FREE(retries_q);
			}
		}
	}

	/* increment flush counter */
	mon_pdev->tx_capture.ppdu_flush_count++;
	dp_tx_capture_info("now_ms[%u] proc_ms[%u] delta[%u] stats_Q[%d]\n",
			   now_ms,
			   ptr_tx_cap->last_processed_ms,
			   delta_ms, is_stats_queue_empty);
}

/**
 * dp_tx_ppdu_stats_process - Deferred PPDU stats handler
 * @context: Opaque work context (PDEV)
 *
 * Return: none
 */
void dp_tx_ppdu_stats_process(void *context)
{
	uint32_t curr_sched_cmdid;
	uint32_t last_ppdu_id;
	uint32_t ppdu_cnt;
	uint32_t ppdu_desc_cnt = 0;
	struct dp_pdev *pdev = (struct dp_pdev *)context;
	struct ppdu_info *ppdu_info, *tmp_ppdu_info = NULL;
	struct ppdu_info *sched_ppdu_info = NULL;

	STAILQ_HEAD(, ppdu_info) sched_ppdu_queue;

	struct ppdu_info *sched_ppdu_list_last_ptr;
	struct dp_tx_cap_nbuf_list *nbuf_ppdu_list;
	struct dp_tx_cap_nbuf_list *ptr_nbuf_list;
	qdf_nbuf_t tmp_nbuf;
	qdf_nbuf_t nbuf_ppdu;
	size_t nbuf_list_sz;
	uint8_t user_mode;
	bool is_stats_queue_empty = false;
	uint32_t tlv_bitmap = 0;
	struct dp_mon_pdev *mon_pdev = pdev->monitor_pdev;
	struct dp_pdev_tx_capture *ptr_tx_cap = &mon_pdev->tx_capture;

	STAILQ_INIT(&sched_ppdu_queue);
	/* Move the PPDU entries to defer list */
	qdf_spin_lock_bh(&ptr_tx_cap->ppdu_stats_lock);
	STAILQ_CONCAT(&ptr_tx_cap->ppdu_stats_defer_queue,
		      &ptr_tx_cap->ppdu_stats_queue);
	ptr_tx_cap->ppdu_stats_defer_queue_depth +=
		ptr_tx_cap->ppdu_stats_queue_depth;
	ptr_tx_cap->ppdu_stats_queue_depth = 0;
	qdf_spin_unlock_bh(&ptr_tx_cap->ppdu_stats_lock);

	is_stats_queue_empty =
		STAILQ_EMPTY(&ptr_tx_cap->ppdu_stats_defer_queue);
	/*
	 * if any chance defer queue is empty, mode change
	 * check need to be done
	 */
	if (is_stats_queue_empty) {
		/* get user mode */
		user_mode =
			qdf_atomic_read(&mon_pdev->tx_capture.tx_cap_usr_mode);
		/*
		 * invoke mode change if user mode value is
		 * different from driver mode value,
		 * this was done to reduce config lock
		 */
		if (user_mode != mon_pdev->tx_capture_enabled)
			dp_enh_tx_cap_mode_change(pdev, user_mode);
	}

	/*
	 * check and flush any pending queue and release queue if it
	 * get build up
	 */
	if (mon_pdev->tx_capture_enabled != CDP_TX_ENH_CAPTURE_DISABLED)
		dp_pdev_tx_cap_flush(pdev, is_stats_queue_empty);

	while (!STAILQ_EMPTY(&ptr_tx_cap->ppdu_stats_defer_queue)) {
		ppdu_info =
			STAILQ_FIRST(&ptr_tx_cap->ppdu_stats_defer_queue);
		curr_sched_cmdid = ppdu_info->sched_cmdid;

		ppdu_cnt = 0;
		STAILQ_FOREACH_SAFE(ppdu_info,
				    &ptr_tx_cap->ppdu_stats_defer_queue,
				    ppdu_info_queue_elem, tmp_ppdu_info) {
			if (curr_sched_cmdid != ppdu_info->sched_cmdid)
				break;
			sched_ppdu_list_last_ptr = ppdu_info;
			ppdu_cnt++;
		}
		if (ppdu_info && (curr_sched_cmdid == ppdu_info->sched_cmdid))
			break;

		last_ppdu_id = sched_ppdu_list_last_ptr->ppdu_id;

		STAILQ_FIRST(&sched_ppdu_queue) =
			STAILQ_FIRST(&ptr_tx_cap->ppdu_stats_defer_queue);
		STAILQ_REMOVE_HEAD_UNTIL(&ptr_tx_cap->ppdu_stats_defer_queue,
					 sched_ppdu_list_last_ptr,
					 ppdu_info_queue_elem);
		STAILQ_NEXT(sched_ppdu_list_last_ptr,
			    ppdu_info_queue_elem) = NULL;

		ptr_tx_cap->ppdu_stats_defer_queue_depth -= ppdu_cnt;

		nbuf_list_sz = sizeof(struct dp_tx_cap_nbuf_list);
		nbuf_ppdu_list = (struct dp_tx_cap_nbuf_list *)
					qdf_mem_malloc(nbuf_list_sz * ppdu_cnt);

		/*
		 * if there is no memory allocated we need to free sched ppdu
		 * list, no ppdu stats will be updated.
		 */
		if (!nbuf_ppdu_list) {
			STAILQ_FOREACH_SAFE(sched_ppdu_info,
					    &sched_ppdu_queue,
					    ppdu_info_queue_elem,
					    tmp_ppdu_info) {
				ppdu_info = sched_ppdu_info;
				tmp_nbuf = ppdu_info->nbuf;
				qdf_mem_free(ppdu_info);
				qdf_nbuf_free(tmp_nbuf);
			}
			continue;
		}

		/*
		 * value stored for debugging purpose to get info
		 * for debugging purpose.
		 */
		ptr_tx_cap->last_nbuf_ppdu_list = nbuf_ppdu_list;
		ptr_tx_cap->last_nbuf_ppdu_list_arr_sz = ppdu_cnt;

		ptr_tx_cap->last_processed_ms =
			qdf_system_ticks_to_msecs(qdf_system_ticks());
		ppdu_desc_cnt = 0;
		STAILQ_FOREACH_SAFE(sched_ppdu_info,
				    &sched_ppdu_queue,
				    ppdu_info_queue_elem, tmp_ppdu_info) {
			ppdu_info = sched_ppdu_info;
			pdev->stats.tx_ppdu_proc++;

			/* update ppdu desc user stats */
			dp_ppdu_desc_user_stats_update(pdev, ppdu_info);
			/*
			 * While processing/corelating Tx buffers, we should
			 * hold the entire PPDU list for the give sched_cmdid
			 * instead of freeing below.
			 */
			nbuf_ppdu = ppdu_info->nbuf;
			tlv_bitmap = ppdu_info->tlv_bitmap;
			qdf_mem_free(ppdu_info);

			qdf_assert_always(nbuf_ppdu);

			/* check tx capture disable */
			if (mon_pdev->tx_capture_enabled ==
			    CDP_TX_ENH_CAPTURE_DISABLED) {
				struct cdp_tx_completion_ppdu *ppdu_desc;

				ppdu_desc = (struct cdp_tx_completion_ppdu *)
					qdf_nbuf_data(nbuf_ppdu);
				/**
				 * Deliver PPDU stats only for valid (acked)
				 * data frames if sniffer mode is not enabled.
				 * If sniffer mode is enabled,
				 * PPDU stats for all frames including
				 * mgmt/control frames should be delivered
				 * to upper layer
				 */
				if (mon_pdev->tx_sniffer_enable ||
				    mon_pdev->mcopy_mode) {
					dp_wdi_event_handler(
							WDI_EVENT_TX_PPDU_DESC,
							pdev->soc,
							nbuf_ppdu,
							HTT_INVALID_PEER,
							WDI_NO_VAL,
							pdev->pdev_id);
				} else {
					if ((ppdu_desc->num_mpdu != 0 ||
					     ppdu_desc->delayed_ba) &&
					    ppdu_desc->num_users != 0 &&
					    ((ppdu_desc->frame_ctrl &
					      HTT_FRAMECTRL_DATATYPE) ||
					     (ppdu_desc->htt_frame_type ==
					      HTT_STATS_FTYPE_SGEN_MU_BAR) ||
					     (ppdu_desc->htt_frame_type ==
					      HTT_STATS_FTYPE_SGEN_BAR)) &&
					    (tlv_bitmap & 1 <<
					     HTT_PPDU_STATS_USR_RATE_TLV)) {
						dp_wdi_event_handler(
							WDI_EVENT_TX_PPDU_DESC,
							pdev->soc,
							nbuf_ppdu,
							HTT_INVALID_PEER,
							WDI_NO_VAL,
							pdev->pdev_id);
					} else {
						qdf_nbuf_free(nbuf_ppdu);
					}
				}

				continue;
			} else {
				tx_cap_debugfs_log_ppdu_desc(pdev, nbuf_ppdu);

				/* process ppdu_info on tx capture turned on */
				ppdu_desc_cnt = dp_tx_cap_proc_per_ppdu_info(
							pdev,
							nbuf_ppdu,
							nbuf_ppdu_list,
							ppdu_desc_cnt);
			}
		}

		/*
		 * At this point we have mpdu queued per ppdu_desc
		 * based on packet capture flags send mpdu info to upper stack
		 */
		if (ppdu_desc_cnt) {
			uint32_t i;

			dp_check_ppdu_and_deliver(pdev, nbuf_ppdu_list,
						  ppdu_desc_cnt);

			for (i = 0; i < ppdu_desc_cnt; i++) {
				ptr_nbuf_list = &nbuf_ppdu_list[i];

				if (dp_tx_cap_nbuf_list_get_ref(
							ptr_nbuf_list)) {
					dp_tx_capture_alert("%pK: missing handling of ppdu_desc ref_cnt:%d ,i : %d ptr %p, ppdu_desc_cnt %d!!!\n",
							    pdev->soc,
							    ptr_nbuf_list->ref_cnt, i, ptr_nbuf_list, ppdu_desc_cnt);
					QDF_BUG(0);
				}
			}
		}

		qdf_mem_free(nbuf_ppdu_list);

		/* get user mode */
		user_mode =
			qdf_atomic_read(&mon_pdev->tx_capture.tx_cap_usr_mode);
		/*
		 * invoke mode change if user mode value is
		 * different from driver mode value,
		 * this was done to reduce config lock
		 */
		if (user_mode != mon_pdev->tx_capture_enabled)
			dp_enh_tx_cap_mode_change(pdev, user_mode);
	}
}

/**
 * dp_ppdu_desc_deliver_1_0(): Function to deliver Tx PPDU status descriptor
 * to upper layer
 * @pdev: DP pdev handle
 * @ppdu_info: per PPDU TLV descriptor
 *
 * return: void
 */
void dp_ppdu_desc_deliver_1_0(struct dp_pdev *pdev,
			      struct ppdu_info *ppdu_info)
{
	struct ppdu_info *s_ppdu_info = NULL;
	struct ppdu_info *ppdu_info_next = NULL;
	struct cdp_tx_completion_ppdu *ppdu_desc = NULL;
	uint32_t time_delta = 0;
	bool starved = 0;
	bool matched = 0;
	bool recv_ack_ba_done = 0;
	struct dp_mon_pdev *mon_pdev = pdev->monitor_pdev;

	if (ppdu_info->tlv_bitmap &
	    (1 << HTT_PPDU_STATS_USR_COMPLTN_ACK_BA_STATUS_TLV) &&
	    ppdu_info->done)
		recv_ack_ba_done = 1;

	mon_pdev->last_sched_cmdid = ppdu_info->sched_cmdid;
	s_ppdu_info = TAILQ_FIRST(&mon_pdev->sched_comp_ppdu_list);

	TAILQ_FOREACH_SAFE(s_ppdu_info, &mon_pdev->sched_comp_ppdu_list,
			   ppdu_info_list_elem, ppdu_info_next) {
		if (s_ppdu_info->tsf_l32 > ppdu_info->tsf_l32)
			time_delta = (MAX_TSF_32 - s_ppdu_info->tsf_l32) +
					ppdu_info->tsf_l32;
		else
			time_delta = ppdu_info->tsf_l32 - s_ppdu_info->tsf_l32;

		if (!s_ppdu_info->done && !recv_ack_ba_done) {
			if (time_delta < MAX_SCHED_STARVE) {
				dp_tx_capture_info("%pK: pdev[%d] ppdu_id[0x%x %d] sched_cmdid[0x%x %d] TLV_B[0x%x] TSF[%u] D[%d]",
						   pdev->soc, pdev->pdev_id,
						   s_ppdu_info->ppdu_id,
						   s_ppdu_info->ppdu_id,
						   s_ppdu_info->sched_cmdid,
						   s_ppdu_info->sched_cmdid,
						   s_ppdu_info->tlv_bitmap,
						   s_ppdu_info->tsf_l32,
						   s_ppdu_info->done);
				break;
			} else {
				starved = 1;
			}
		}

		mon_pdev->delivered_sched_cmdid = s_ppdu_info->sched_cmdid;
		TAILQ_REMOVE(&mon_pdev->sched_comp_ppdu_list, s_ppdu_info,
			     ppdu_info_list_elem);
		mon_pdev->sched_comp_list_depth--;

		ppdu_desc = (struct cdp_tx_completion_ppdu *)
				qdf_nbuf_data(s_ppdu_info->nbuf);
		ppdu_desc->tlv_bitmap = s_ppdu_info->tlv_bitmap;
		ppdu_desc->sched_cmdid = ppdu_info->sched_cmdid;

		if (starved) {
			dp_tx_capture_info("%pK: ppdu starved fc[0x%x] h_ftype[%d] tlv_bitmap[0x%x] cs[%d]\n",
					   pdev->soc, ppdu_desc->frame_ctrl,
					   ppdu_desc->htt_frame_type,
					   ppdu_desc->tlv_bitmap,
					   ppdu_desc->user[0].completion_status);
			starved = 0;
		}

		if (ppdu_info->ppdu_id == s_ppdu_info->ppdu_id &&
		    ppdu_info->sched_cmdid == s_ppdu_info->sched_cmdid)
			matched = 1;

		dp_tx_capture_info("%pK: pdev[%d] vdev[%d] ppdu_id[0x%x %d] sched_cmdid[0x%x %d] FC[0x%x] H_FTYPE[0x%x] TLV_B[0x%x] TSF[%u] cs[%d] M[%d] R_PID[%d S %d]",
				   pdev->soc, pdev->pdev_id, ppdu_desc->vdev_id,
				   s_ppdu_info->ppdu_id, s_ppdu_info->ppdu_id,
				   s_ppdu_info->sched_cmdid,
				   s_ppdu_info->sched_cmdid,
				   ppdu_desc->frame_ctrl,
				   ppdu_desc->htt_frame_type,
				   ppdu_desc->tlv_bitmap, s_ppdu_info->tsf_l32,
				   ppdu_desc->user[0].completion_status,
				   matched,
				   ppdu_info->ppdu_id, ppdu_info->sched_cmdid);

		qdf_spin_lock_bh(&mon_pdev->tx_capture.ppdu_stats_lock);

		if (qdf_unlikely(!mon_pdev->tx_capture_enabled &&
				 (mon_pdev->tx_capture.ppdu_stats_queue_depth +
				  mon_pdev->tx_capture.ppdu_stats_defer_queue_depth) >
				 DP_TX_PPDU_PROC_MAX_DEPTH)) {
			qdf_nbuf_free(s_ppdu_info->nbuf);
			qdf_mem_free(s_ppdu_info);
			mon_pdev->tx_capture.ppdu_dropped++;
		} else {
			STAILQ_INSERT_TAIL(&mon_pdev->tx_capture.ppdu_stats_queue,
					   s_ppdu_info, ppdu_info_queue_elem);
			mon_pdev->tx_capture.ppdu_stats_queue_depth++;
		}
		qdf_spin_unlock_bh(&mon_pdev->tx_capture.ppdu_stats_lock);

		if (matched)
			break;
	}

	/* update timestamp of last received ppdu stats tlv */
	mon_pdev->tx_capture.ppdu_stats_ms =
				qdf_system_ticks_to_msecs(qdf_system_ticks());

	if (mon_pdev->tx_capture.ppdu_stats_queue_depth >
		DP_TX_PPDU_PROC_THRESHOLD) {
		qdf_queue_work(0, mon_pdev->tx_capture.ppdu_stats_workqueue,
			       &mon_pdev->tx_capture.ppdu_stats_work);
	}

	if (!mon_pdev->stop_tx_capture_work_q_timer) {
		qdf_timer_mod(&mon_pdev->tx_capture.work_q_timer,
			      TX_CAPTURE_WORK_Q_TIMER_MS);
	}
}

static void set_mpdu_info(
	struct cdp_tx_indication_info *tx_capture_info,
	struct mon_rx_status *rx_status,
	struct mon_rx_user_status *rx_user_status)
{
	struct cdp_tx_indication_mpdu_info *mpdu_info;

	qdf_mem_set(tx_capture_info,
		    sizeof(struct cdp_tx_indication_info), 0);

	mpdu_info = &tx_capture_info->mpdu_info;
	mpdu_info->ppdu_start_timestamp = rx_status->tsft + 16;
	mpdu_info->channel_num = rx_status->chan_num;
	mpdu_info->channel = rx_status->chan_freq;

	mpdu_info->bw = 0;

	if (rx_status->preamble_type == HAL_RX_PKT_TYPE_11B) {
		mpdu_info->preamble = DOT11_B;
		mpdu_info->mcs = CDP_LEGACY_MCS3;
	} else if (rx_status->preamble_type == HAL_RX_PKT_TYPE_11A) {
		mpdu_info->preamble = DOT11_A;
		mpdu_info->mcs = CDP_LEGACY_MCS3;
	} else {
		mpdu_info->preamble = DOT11_A;
		mpdu_info->mcs = CDP_LEGACY_MCS1;
	}
}

static void dp_gen_ack_frame(struct hal_rx_ppdu_info *ppdu_info,
			     struct dp_peer *peer,
			     qdf_nbuf_t mpdu_nbuf)
{
	struct ieee80211_frame_min_one *wh_addr1;

	wh_addr1 = (struct ieee80211_frame_min_one *)
		qdf_nbuf_data(mpdu_nbuf);

	wh_addr1->i_fc[0] = 0;
	wh_addr1->i_fc[1] = 0;
	wh_addr1->i_fc[0] =  IEEE80211_FC0_VERSION_0 |
		IEEE80211_FC0_TYPE_CTL |
		IEEE80211_FC0_SUBTYPE_ACK;
	if (peer) {
		qdf_mem_copy(wh_addr1->i_addr1,
			     &peer->mac_addr.raw[0],
			     QDF_MAC_ADDR_SIZE);
	} else {
		qdf_mem_copy(wh_addr1->i_addr1,
			     &ppdu_info->nac_info.mac_addr2[0],
			     QDF_MAC_ADDR_SIZE);
	}
	*(u_int16_t *)(&wh_addr1->i_dur) = qdf_cpu_to_le16(0x0000);
	qdf_nbuf_set_pktlen(mpdu_nbuf, sizeof(*wh_addr1));
}

static void dp_gen_block_ack_frame(
	struct hal_rx_ppdu_info *ppdu_info,
	struct mon_rx_user_status *rx_user_status,
	struct mon_rx_user_info *rx_user_info,
	struct dp_peer *peer,
	qdf_nbuf_t mpdu_nbuf)
{
	struct dp_vdev *vdev = NULL;
	uint32_t tid;
	struct dp_tx_tid *tx_tid;
	struct ieee80211_ctlframe_addr2 *wh_addr2;
	uint8_t *frm;

	tid = rx_user_status->tid;
	tx_tid = &peer->monitor_peer->tx_capture.tx_tid[tid];
	if (ppdu_info->sw_frame_group_id != HAL_MPDU_SW_FRAME_GROUP_CTRL_BAR) {
		tx_tid->first_data_seq_ctrl =
			rx_user_status->first_data_seq_ctrl;
		tx_tid->mpdu_cnt = rx_user_status->mpdu_cnt_fcs_ok +
			rx_user_status->mpdu_cnt_fcs_err;
		if (tx_tid->mpdu_cnt > DP_MAX_MPDU_64)
			qdf_mem_copy(tx_tid->mpdu_fcs_ok_bitmap,
				     rx_user_status->mpdu_fcs_ok_bitmap,
				     HAL_RX_NUM_WORDS_PER_PPDU_BITMAP * sizeof(
				     rx_user_status->mpdu_fcs_ok_bitmap[0]));
		else
			qdf_mem_copy(tx_tid->mpdu_fcs_ok_bitmap,
				     rx_user_status->mpdu_fcs_ok_bitmap,
				     DP_NUM_WORDS_PER_PPDU_BITMAP_64 * sizeof(
				     rx_user_status->mpdu_fcs_ok_bitmap[0]));
	}

	wh_addr2 = (struct ieee80211_ctlframe_addr2 *)
			qdf_nbuf_data(mpdu_nbuf);

	qdf_mem_zero(wh_addr2, DP_BA_ACK_FRAME_SIZE);

	wh_addr2->i_fc[0] = 0;
	wh_addr2->i_fc[1] = 0;
	wh_addr2->i_fc[0] =  IEEE80211_FC0_VERSION_0 |
		IEEE80211_FC0_TYPE_CTL |
		IEEE80211_FC0_BLOCK_ACK;
	*(u_int16_t *)(&wh_addr2->i_aidordur) = qdf_cpu_to_le16(0x0000);

	vdev = peer->vdev;
	if (vdev)
		qdf_mem_copy(wh_addr2->i_addr2, vdev->mac_addr.raw,
			     QDF_MAC_ADDR_SIZE);
	qdf_mem_copy(wh_addr2->i_addr1, &peer->mac_addr.raw[0],
		     QDF_MAC_ADDR_SIZE);

	frm = (uint8_t *)&wh_addr2[1];
	*((uint16_t *)frm) =
		qdf_cpu_to_le16((rx_user_status->tid <<
		DP_IEEE80211_BAR_CTL_TID_S) |
		DP_IEEE80211_BAR_CTL_COMBA);
	frm += 2;
	*((uint16_t *)frm) =
		tx_tid->first_data_seq_ctrl;
	frm += 2;
	if (tx_tid->mpdu_cnt > DP_MAX_MPDU_64) {
		qdf_mem_copy(frm,
			     tx_tid->mpdu_fcs_ok_bitmap,
			     HAL_RX_NUM_WORDS_PER_PPDU_BITMAP *
			     sizeof(rx_user_status->mpdu_fcs_ok_bitmap[0]));
		frm += DP_NUM_BYTES_PER_PPDU_BITMAP;
	} else {
		qdf_mem_copy(frm,
			     tx_tid->mpdu_fcs_ok_bitmap,
			     DP_NUM_WORDS_PER_PPDU_BITMAP_64 *
			     sizeof(rx_user_status->mpdu_fcs_ok_bitmap[0]));
		frm += DP_NUM_BYTES_PER_PPDU_BITMAP_64;
	}
	qdf_nbuf_set_pktlen(mpdu_nbuf,
			    (frm - (uint8_t *)qdf_nbuf_data(mpdu_nbuf)));
}

static void dp_gen_cts_frame(struct hal_rx_ppdu_info *ppdu_info,
			     struct dp_peer *peer,
			     qdf_nbuf_t mpdu_nbuf)
{
	struct ieee80211_frame_min_one *wh_addr1;
	uint16_t duration;

	wh_addr1 = (struct ieee80211_frame_min_one *)
		qdf_nbuf_data(mpdu_nbuf);

	wh_addr1->i_fc[0] = 0;
	wh_addr1->i_fc[1] = 0;
	wh_addr1->i_fc[0] =  IEEE80211_FC0_VERSION_0 |
		IEEE80211_FC0_TYPE_CTL |
		IEEE80211_FC0_SUBTYPE_CTS;
	qdf_mem_copy(wh_addr1->i_addr1, &peer->mac_addr.raw[0],
		     QDF_MAC_ADDR_SIZE);
	duration = (ppdu_info->rx_status.duration > SIFS_INTERVAL) ?
		ppdu_info->rx_status.duration - SIFS_INTERVAL : 0;
	wh_addr1->i_dur[0] = duration & 0xff;
	wh_addr1->i_dur[1] = (duration >> 8) & 0xff;
	qdf_nbuf_set_pktlen(mpdu_nbuf, sizeof(*wh_addr1));
}

/**
 * dp_send_cts_frame_to_stack(): Function to deliver HW generated CTS frame
 *	in response to RTS
 * @soc: core txrx main context
 * @pdev: DP pdev object
 * @ppdu_info: HAL RX PPDU info retrieved from status ring TLV
 *
 * return: status
 */
QDF_STATUS dp_send_cts_frame_to_stack(struct dp_soc *soc,
				      struct dp_pdev *pdev,
				      struct hal_rx_ppdu_info *ppdu_info)
{
	struct cdp_tx_indication_info tx_capture_info;
	struct mon_rx_user_status *rx_user_status =
		&ppdu_info->rx_user_status[0];
	struct dp_ast_entry *ast_entry;
	uint32_t peer_id;
	struct dp_peer *peer;
	struct dp_vdev *vdev = NULL;
	struct dp_mon_pdev *mon_pdev = pdev->monitor_pdev;

	if (rx_user_status->ast_index >=
	    wlan_cfg_get_max_ast_idx(soc->wlan_cfg_ctx)) {
		return QDF_STATUS_E_FAILURE;
	}

	qdf_spin_lock_bh(&soc->ast_lock);
	ast_entry = soc->ast_table[rx_user_status->ast_index];
	if (!ast_entry) {
		qdf_spin_unlock_bh(&soc->ast_lock);
		return QDF_STATUS_E_FAILURE;
	}

	peer_id = ast_entry->peer_id;
	qdf_spin_unlock_bh(&soc->ast_lock);

	peer = DP_TX_PEER_GET_REF(pdev, peer_id);
	if (!peer)
		return QDF_STATUS_E_FAILURE;

	if (!dp_peer_or_pdev_tx_cap_enabled(pdev, NULL, peer->mac_addr.raw)) {
		DP_TX_PEER_DEL_REF(peer);
		return QDF_STATUS_E_FAILURE;
	}

	if (mon_pdev->tx_capture_enabled ==
	    CDP_TX_ENH_CAPTURE_ENABLE_ALL_PEERS) {
		int8_t match = 0;

		TAILQ_FOREACH(vdev, &pdev->vdev_list, vdev_list_elem) {
			if (!qdf_mem_cmp(vdev->mac_addr.raw,
					 ppdu_info->rx_info.mac_addr1,
					 QDF_MAC_ADDR_SIZE)) {
				match = 1;
				break;
			}
		}
		if (!match) {
			DP_TX_PEER_DEL_REF(peer);
			return QDF_STATUS_E_FAILURE;
		}
	}

	set_mpdu_info(&tx_capture_info,
		      &ppdu_info->rx_status, rx_user_status);
	/* ppdu_desc is not required for legacy frames */
	tx_capture_info.ppdu_desc = NULL;

	tx_capture_info.mpdu_nbuf =
		qdf_nbuf_alloc(pdev->soc->osdev,
			       MAX_MONITOR_HEADER +
			       DP_CTS_FRAME_SIZE,
			       MAX_MONITOR_HEADER,
			       4, FALSE);

	if (!tx_capture_info.mpdu_nbuf) {
		DP_TX_PEER_DEL_REF(peer);
		return QDF_STATUS_E_NOMEM;
	}

	dp_gen_cts_frame(ppdu_info, peer,
			 tx_capture_info.mpdu_nbuf);
	DP_TX_PEER_DEL_REF(peer);
	TX_CAP_WDI_EVENT_HANDLER(pdev->soc, pdev->pdev_id, &tx_capture_info);

	if (tx_capture_info.mpdu_nbuf)
		qdf_nbuf_free(tx_capture_info.mpdu_nbuf);

	return QDF_STATUS_SUCCESS;
}

/**
 * dp_send_usr_ack_frm_to_stack(): Function to generate BA or ACK frame and
 * send to upper layer
 * @soc: core txrx main context
 * @pdev: DP pdev object
 * @ppdu_info: HAL RX PPDU info retrieved from status ring TLV
 * @rx_status: variable for rx status
 * @rx_user_status: variable for rx user status
 * @rx_user_info: variable for rx user info
 *
 * return: no
 */
void dp_send_usr_ack_frm_to_stack(struct dp_soc *soc,
				  struct dp_pdev *pdev,
				  struct hal_rx_ppdu_info *ppdu_info,
				  struct mon_rx_status *rx_status,
				  struct mon_rx_user_status *rx_user_status,
				  struct mon_rx_user_info *rx_user_info)
{
	struct cdp_tx_indication_info tx_capture_info;
	struct dp_peer *peer;
	struct dp_ast_entry *ast_entry;
	uint32_t peer_id;
	uint32_t ast_index;
	uint8_t *ptr_mac_addr;

	if (rx_user_info->qos_control_info_valid &&
	    ((rx_user_info->qos_control &
	    IEEE80211_QOS_ACKPOLICY) >> IEEE80211_QOS_ACKPOLICY_S)
	    == IEEE80211_BAR_CTL_NOACK)
		return;

	ast_index = rx_user_status->ast_index;

	if (ast_index >=
	    wlan_cfg_get_max_ast_idx(soc->wlan_cfg_ctx)) {
		if (ppdu_info->sw_frame_group_id ==
		    HAL_MPDU_SW_FRAME_GROUP_CTRL_BAR)
			return;

		ptr_mac_addr = &ppdu_info->nac_info.mac_addr2[0];
		if (!dp_peer_or_pdev_tx_cap_enabled(pdev,
						    NULL, ptr_mac_addr))
			return;

		if (IEEE80211_IS_ZERO(ppdu_info->nac_info.mac_addr2))
			return;

		set_mpdu_info(&tx_capture_info,
			      rx_status, rx_user_status);
		tx_capture_info.mpdu_nbuf =
			qdf_nbuf_alloc(pdev->soc->osdev,
				       MAX_MONITOR_HEADER +
				       DP_BA_ACK_FRAME_SIZE,
				       MAX_MONITOR_HEADER,
				       4, FALSE);
		if (!tx_capture_info.mpdu_nbuf)
			return;
		dp_gen_ack_frame(ppdu_info, NULL,
				 tx_capture_info.mpdu_nbuf);
		TX_CAP_WDI_EVENT_HANDLER(pdev->soc, pdev->pdev_id,
					 &tx_capture_info);

		if (tx_capture_info.mpdu_nbuf)
			qdf_nbuf_free(tx_capture_info.mpdu_nbuf);
		return;
	}

	qdf_spin_lock_bh(&soc->ast_lock);
	ast_entry = soc->ast_table[ast_index];
	if (!ast_entry) {
		qdf_spin_unlock_bh(&soc->ast_lock);
		return;
	}

	peer_id = ast_entry->peer_id;
	qdf_spin_unlock_bh(&soc->ast_lock);

	peer = DP_TX_PEER_GET_REF(pdev, peer_id);
	if (!peer)
		return;

	if (!dp_peer_or_pdev_tx_cap_enabled(pdev, peer,
					    peer->mac_addr.raw)) {
		DP_TX_PEER_DEL_REF(peer);
		return;
	}

	set_mpdu_info(&tx_capture_info,
		      rx_status, rx_user_status);

	tx_capture_info.mpdu_nbuf =
		qdf_nbuf_alloc(pdev->soc->osdev,
			       MAX_MONITOR_HEADER +
			       DP_BA_ACK_FRAME_SIZE,
			       MAX_MONITOR_HEADER,
			       4, FALSE);

	if (!tx_capture_info.mpdu_nbuf) {
		DP_TX_PEER_DEL_REF(peer);
		return;
	}

	if (peer->rx_tid[rx_user_status->tid].ba_status == DP_RX_BA_ACTIVE ||
	    ppdu_info->sw_frame_group_id == HAL_MPDU_SW_FRAME_GROUP_CTRL_BAR) {
		dp_gen_block_ack_frame(ppdu_info,
				       rx_user_status,
				       rx_user_info,
				       peer,
				       tx_capture_info.mpdu_nbuf);
		tx_capture_info.mpdu_info.tid = rx_user_status->tid;

	} else {
		dp_gen_ack_frame(ppdu_info, peer,
				 tx_capture_info.mpdu_nbuf);
	}

	DP_TX_PEER_DEL_REF(peer);
	TX_CAP_WDI_EVENT_HANDLER(pdev->soc, pdev->pdev_id,
				 &tx_capture_info);

	if (tx_capture_info.mpdu_nbuf)
		qdf_nbuf_free(tx_capture_info.mpdu_nbuf);
}

/**
 * dp_send_ack_frame_to_stack(): Function to generate BA or ACK frame and
 * send to upper layer on received unicast frame
 * @soc: core txrx main context
 * @pdev: DP pdev object
 * @ppdu_info: HAL RX PPDU info retrieved from status ring TLV
 *
 * return: status
 */
QDF_STATUS dp_send_ack_frame_to_stack(struct dp_soc *soc,
				      struct dp_pdev *pdev,
				      struct hal_rx_ppdu_info *ppdu_info)
{
	struct mon_rx_status *rx_status;
	struct mon_rx_user_status *rx_user_status;
	struct mon_rx_user_info *rx_user_info;
	uint32_t i;

	rx_status = &ppdu_info->rx_status;

	if (ppdu_info->sw_frame_group_id ==
	    HAL_MPDU_SW_FRAME_GROUP_CTRL_RTS) {
		return dp_send_cts_frame_to_stack(soc, pdev, ppdu_info);
	}

	if (!rx_status->rxpcu_filter_pass)
		return QDF_STATUS_SUCCESS;

	if (ppdu_info->sw_frame_group_id ==
	    HAL_MPDU_SW_FRAME_GROUP_MGMT_BEACON ||
	    ppdu_info->sw_frame_group_id ==
	    HAL_MPDU_SW_FRAME_GROUP_CTRL_NDPA)
		return QDF_STATUS_SUCCESS;

	if ((ppdu_info->sw_frame_group_id ==
	     HAL_MPDU_SW_FRAME_GROUP_MGMT_PROBE_REQ) &&
	    (ppdu_info->rx_info.mac_addr1[0] & 1)) {
		return QDF_STATUS_SUCCESS;
	}

	if (ppdu_info->sw_frame_group_id == HAL_MPDU_SW_FRAME_GROUP_CTRL_BAR)
		return QDF_STATUS_SUCCESS;

	for (i = 0; i < ppdu_info->com_info.num_users; i++) {
		if (i > OFDMA_NUM_USERS)
			return QDF_STATUS_E_FAULT;

		rx_user_status =  &ppdu_info->rx_user_status[i];
		rx_user_info = &ppdu_info->rx_user_info[i];

		dp_send_usr_ack_frm_to_stack(soc, pdev, ppdu_info, rx_status,
					     rx_user_status, rx_user_info);
	}

	return QDF_STATUS_SUCCESS;
}

/**
 * dp_bar_send_ack_frm_to_stack(): send BA or ACK frame
 * to upper layers on received BAR packet for tx capture feature
 *
 * @soc: soc handle
 * @pdev: pdev handle
 * @nbuf: received packet
 *
 * Return: QDF_STATUS_SUCCESS on success
 *         others on error
 */
QDF_STATUS
dp_bar_send_ack_frm_to_stack(struct dp_soc *soc,
			     struct dp_pdev *pdev,
			     qdf_nbuf_t nbuf)
{
	struct ieee80211_ctlframe_addr2 *wh;
	uint8_t *frm;
	struct hal_rx_ppdu_info *ppdu_info;
	struct mon_rx_status *rx_status;
	struct mon_rx_user_status *rx_user_status;
	struct mon_rx_user_info *rx_user_info;
	uint16_t bar_ctl;
	uint32_t user_id;
	uint8_t tid;
	qdf_frag_t addr;
	struct dp_mon_pdev *mon_pdev = pdev->monitor_pdev;

	if (!nbuf)
		return QDF_STATUS_E_INVAL;

	/* Get addr pointing to 80211 header */
	addr = dp_rx_mon_get_nbuf_80211_hdr(nbuf);
	if (qdf_unlikely(!addr)) {
		dp_tx_capture_err("%pK: Unable to get 80211 header address",
				  soc);
		return QDF_STATUS_E_INVAL;
	}

	wh = (struct ieee80211_ctlframe_addr2 *)addr;

	if (wh->i_fc[0] != (IEEE80211_FC0_VERSION_0 |
	     IEEE80211_FC0_TYPE_CTL | IEEE80211_FC0_SUBTYPE_BAR)) {
		return QDF_STATUS_SUCCESS;
	}

	frm = (uint8_t *)&wh[1];

	bar_ctl = qdf_le16_to_cpu(*(uint16_t *)frm);

	if (bar_ctl & DP_IEEE80211_BAR_CTL_POLICY_M)
		return QDF_STATUS_SUCCESS;

	tid = (bar_ctl >> DP_IEEE80211_BAR_CTL_TID_S) &
		DP_IEEE80211_BAR_CTL_TID_M;

	ppdu_info = &mon_pdev->ppdu_info;
	user_id = ppdu_info->rx_info.user_id;
	rx_status = &ppdu_info->rx_status;
	rx_user_status =  &ppdu_info->rx_user_status[user_id];
	rx_user_info = &ppdu_info->rx_user_info[user_id];
	rx_user_status->tid = tid;

	dp_send_usr_ack_frm_to_stack(soc, pdev, ppdu_info, rx_status,
				     rx_user_status, rx_user_info);

	return QDF_STATUS_SUCCESS;
}

/**
 * dp_gen_noack_frame: generate noack Action frame by using parameters
 *					from received NDPA frame
 * @ppdu_info: pointer to ppdu_info
 * @peer: pointer to peer structure
 * @mpdu_nbuf: buffer for the generated noack frame
 * @mon_mpdu: mpdu from monitor destination path
 *
 * Return: QDF_STATUS
 */
static void dp_gen_noack_frame(struct hal_rx_ppdu_info *ppdu_info,
			       struct dp_peer *peer, qdf_nbuf_t mpdu_nbuf,
			       qdf_nbuf_t mon_mpdu)
{
	struct ieee80211_frame *wh;
	uint16_t duration;
	struct dp_vdev *vdev = NULL;
	uint8_t token = 0;
	uint8_t *frm;
	char *ndpa_buf = NULL;

	/* Get addr pointing to 80211 header */
	ndpa_buf = dp_rx_mon_get_nbuf_80211_hdr(mon_mpdu);
	if (qdf_unlikely(!ndpa_buf)) {
		dp_tx_capture_err("Unable to get 80211 header address");
		return;
	}

	wh = (struct ieee80211_frame *)qdf_nbuf_data(mpdu_nbuf);

	qdf_mem_zero(((char *)wh), DP_ACKNOACK_FRAME_SIZE);

	wh->i_fc[0] = IEEE80211_FC0_VERSION_0 |
			IEEE80211_FC0_TYPE_MGT |
			IEEE80211_FCO_SUBTYPE_ACTION_NO_ACK;

	qdf_mem_copy(wh->i_addr1, &peer->mac_addr.raw[0], QDF_MAC_ADDR_SIZE);

	vdev = peer->vdev;
	if (vdev) {
		qdf_mem_copy(wh->i_addr2,
			     vdev->mac_addr.raw,
			     QDF_MAC_ADDR_SIZE);

		qdf_mem_copy(wh->i_addr3,
			     vdev->mac_addr.raw,
			     QDF_MAC_ADDR_SIZE);
	}

	duration = (ppdu_info->rx_status.duration > SIFS_INTERVAL) ?
		    ppdu_info->rx_status.duration - SIFS_INTERVAL : 0;
	wh->i_dur[0] = duration & 0xff;
	wh->i_dur[1] = (duration >> 8) & 0xff;

	frm = (uint8_t *)&wh[1];
	/*
	 * Update category field
	 */
	*frm = DP_IEEE80211_CATEGORY_VHT;

	/*
	 * Update sounding token obtained from NDPA,
	 * shift to get upper six bits
	 */
	frm += DP_NOACK_SOUNDING_TOKEN_POS;

	token = ndpa_buf[DP_NDPA_TOKEN_POS] >> DP_NOACK_STOKEN_POS_SHIFT;

	*frm = (token) << DP_NOACK_STOKEN_POS_SHIFT;

	qdf_nbuf_set_pktlen(mpdu_nbuf, DP_ACKNOACK_FRAME_SIZE);
}

/**
 * dp_send_noack_frame_to_stack: Sends noack Action frame to upper stack
 *					in response to received NDPA frame.
 * @soc: SoC handle
 * @pdev: PDEV pointer
 * @mon_mpdu: mpdu from monitor destination path
 *
 * Return: QDF_STATUS
 */
QDF_STATUS dp_send_noack_frame_to_stack(struct dp_soc *soc,
					struct dp_pdev *pdev,
					qdf_nbuf_t mon_mpdu)
{
	struct dp_mon_pdev *mon_pdev = pdev->monitor_pdev;
	struct hal_rx_ppdu_info *ppdu_info = &mon_pdev->ppdu_info;
	struct mon_rx_user_status *rx_user_status =
				&ppdu_info->rx_user_status[0];
	struct dp_ast_entry *ast_entry;
	uint32_t peer_id;
	struct dp_peer *peer;
	struct cdp_tx_indication_info tx_capture_info;

	if (rx_user_status->ast_index >=
		wlan_cfg_get_max_ast_idx(soc->wlan_cfg_ctx)) {
		return QDF_STATUS_E_FAILURE;
	}

	qdf_spin_lock_bh(&soc->ast_lock);
	ast_entry = soc->ast_table[rx_user_status->ast_index];
	if (!ast_entry) {
		qdf_spin_unlock_bh(&soc->ast_lock);
		return QDF_STATUS_E_FAILURE;
	}

	peer_id = ast_entry->peer_id;
	qdf_spin_unlock_bh(&soc->ast_lock);

	peer = DP_TX_PEER_GET_REF(pdev, peer_id);
	if (!peer)
		return QDF_STATUS_E_FAILURE;

	if (!dp_peer_or_pdev_tx_cap_enabled(pdev, peer, peer->mac_addr.raw)) {
		DP_TX_PEER_DEL_REF(peer);
		return QDF_STATUS_E_FAILURE;
	}

	set_mpdu_info(&tx_capture_info,
		      &ppdu_info->rx_status, rx_user_status);

	tx_capture_info.mpdu_info.mcs = rx_user_status->mcs;
	/*
	 *ppdu_desc is not required for legacy frames
	 */
	tx_capture_info.ppdu_desc = NULL;

	tx_capture_info.mpdu_nbuf =
		qdf_nbuf_alloc(pdev->soc->osdev,
			       MAX_MONITOR_HEADER +
			       DP_ACKNOACK_FRAME_SIZE,
			       MAX_MONITOR_HEADER,
			       4, FALSE);

	if (!tx_capture_info.mpdu_nbuf) {
		DP_TX_PEER_DEL_REF(peer);
		return QDF_STATUS_E_NOMEM;
	}

	dp_gen_noack_frame(ppdu_info, peer,
			   tx_capture_info.mpdu_nbuf, mon_mpdu);

	DP_TX_PEER_DEL_REF(peer);
	TX_CAP_WDI_EVENT_HANDLER(pdev->soc, pdev->pdev_id, &tx_capture_info);

	if (tx_capture_info.mpdu_nbuf)
		qdf_nbuf_free(tx_capture_info.mpdu_nbuf);

	return QDF_STATUS_SUCCESS;
}

/**
 * dp_handle_tx_capture_from_dest: Handle any TX capture frames from
 *			monitor destination path.
 * @soc: SoC handle
 * @pdev: PDEV pointer
 * @mon_mpdu: mpdu from monitor destination path
 *
 * Return: QDF_STATUS
 */
QDF_STATUS dp_handle_tx_capture_from_dest(struct dp_soc *soc,
					  struct dp_pdev *pdev,
					  qdf_nbuf_t mon_mpdu)
{
	struct dp_mon_pdev *mon_pdev = pdev->monitor_pdev;
	struct hal_rx_ppdu_info *ppdu_info = &mon_pdev->ppdu_info;

	/*
	 * The below switch case can be extended to
	 * add more frame types as needed
	 */
	switch (ppdu_info->sw_frame_group_id) {
	case HAL_MPDU_SW_FRAME_GROUP_CTRL_NDPA:
		return dp_send_noack_frame_to_stack(soc, pdev, mon_mpdu);

	case HAL_MPDU_SW_FRAME_GROUP_CTRL_BAR:
		return dp_bar_send_ack_frm_to_stack(soc, pdev, mon_mpdu);

	default:
		break;
	}
	return QDF_STATUS_SUCCESS;
}

/**
 * dp_peer_set_tx_capture_enabled_1_0: Set tx_cap_enabled bit in peer
 * @pdev: DP PDEV handle
 * @peer: Peer handle
 * @value: Enable/disable setting for tx_cap_enabled
 * @peer_mac: peer mac address
 *
 * Return: QDF_STATUS
 */
QDF_STATUS
dp_peer_set_tx_capture_enabled_1_0(struct dp_pdev *pdev,
				   struct dp_peer *peer, uint8_t value,
				   uint8_t *peer_mac)
{
	uint32_t peer_id = HTT_INVALID_PEER;
	QDF_STATUS status = QDF_STATUS_E_FAILURE;

	if (value) {
		if (dp_peer_tx_cap_add_filter(pdev, peer_id, peer_mac)) {
			if (peer && peer->monitor_peer)
				peer->monitor_peer->tx_cap_enabled = value;
			status = QDF_STATUS_SUCCESS;
		}
	} else {
		if (dp_peer_tx_cap_del_filter(pdev, peer_id, peer_mac)) {
			if (peer && peer->monitor_peer)
				peer->monitor_peer->tx_cap_enabled = value;
			status = QDF_STATUS_SUCCESS;
		}
	}

	return status;
}

/*
 * dp_peer_tx_capture_filter_check_1_0: check filter is enable for the filter
 * and update tx_cap_enabled flag
 * @pdev: DP PDEV handle
 * @peer: DP PEER handle
 *
 * return: void
 */
void dp_peer_tx_capture_filter_check_1_0(struct dp_pdev *pdev,
					 struct dp_peer *peer)
{
	if (!peer || !peer->monitor_peer)
		return;

	if (dp_peer_tx_cap_search(pdev, peer->peer_id,
				  peer->mac_addr.raw)) {
		peer->monitor_peer->tx_cap_enabled = 1;
	}
}

/*
 * dbg_copy_ppdu_desc: copy ppdu_desc to tx capture debugfs
 * @nbuf_ppdu: nbuf ppdu
 *
 * return: void
 */
void *dbg_copy_ppdu_desc(qdf_nbuf_t nbuf_ppdu)
{
	struct dbg_tx_comp_ppdu *ptr_dbg_ppdu = NULL;
	struct cdp_tx_completion_ppdu *ppdu_desc = NULL;
	uint8_t i = 0;
	uint8_t num_users = 0;

	ppdu_desc = (struct cdp_tx_completion_ppdu *)
			qdf_nbuf_data(nbuf_ppdu);

	ptr_dbg_ppdu = qdf_mem_malloc(sizeof(struct dbg_tx_comp_ppdu) +
				      (ppdu_desc->num_users *
				       sizeof(struct dbg_tx_comp_ppdu_user)));
	if (!ptr_dbg_ppdu)
		return NULL;

	num_users = ppdu_desc->num_users;

	ptr_dbg_ppdu->ppdu_id = ppdu_desc->ppdu_id;
	ptr_dbg_ppdu->bar_ppdu_id = ppdu_desc->bar_ppdu_id;
	ptr_dbg_ppdu->vdev_id = ppdu_desc->vdev_id;
	ptr_dbg_ppdu->bar_num_users = ppdu_desc->bar_num_users;
	ptr_dbg_ppdu->num_users = ppdu_desc->num_users;
	ptr_dbg_ppdu->sched_cmdid = ppdu_desc->sched_cmdid;
	ptr_dbg_ppdu->tlv_bitmap = ppdu_desc->tlv_bitmap;
	ptr_dbg_ppdu->htt_frame_type = ppdu_desc->htt_frame_type;
	ptr_dbg_ppdu->frame_ctrl = ppdu_desc->frame_ctrl;
	ptr_dbg_ppdu->ppdu_start_timestamp = ppdu_desc->ppdu_start_timestamp;
	ptr_dbg_ppdu->ppdu_end_timestamp = ppdu_desc->ppdu_end_timestamp;
	ptr_dbg_ppdu->bar_ppdu_start_timestamp =
					ppdu_desc->bar_ppdu_start_timestamp;
	ptr_dbg_ppdu->bar_ppdu_end_timestamp =
					ppdu_desc->bar_ppdu_end_timestamp;

	for (i = 0; i < ppdu_desc->num_users; i++) {
		ptr_dbg_ppdu->user[i].completion_status =
					ppdu_desc->user[i].completion_status;
		ptr_dbg_ppdu->user[i].tid = ppdu_desc->user[i].tid;
		ptr_dbg_ppdu->user[i].peer_id = ppdu_desc->user[i].peer_id;

		qdf_mem_copy(ptr_dbg_ppdu->user[i].mac_addr,
			     ppdu_desc->user[i].mac_addr, 6);
		ptr_dbg_ppdu->user[i].frame_ctrl =
					ppdu_desc->user[i].frame_ctrl;
		ptr_dbg_ppdu->user[i].qos_ctrl = ppdu_desc->user[i].qos_ctrl;
		ptr_dbg_ppdu->user[i].mpdu_tried =
					ppdu_desc->user[i].mpdu_tried_ucast +
					ppdu_desc->user[i].mpdu_tried_mcast;
		ptr_dbg_ppdu->user[i].mpdu_success =
					ppdu_desc->user[i].mpdu_success;
		ptr_dbg_ppdu->user[i].ltf_size = ppdu_desc->user[i].ltf_size;
		ptr_dbg_ppdu->user[i].stbc = ppdu_desc->user[i].stbc;
		ptr_dbg_ppdu->user[i].he_re = ppdu_desc->user[i].he_re;
		ptr_dbg_ppdu->user[i].txbf = ppdu_desc->user[i].txbf;
		ptr_dbg_ppdu->user[i].bw = ppdu_desc->user[i].bw;
		ptr_dbg_ppdu->user[i].nss = ppdu_desc->user[i].nss;
		ptr_dbg_ppdu->user[i].mcs = ppdu_desc->user[i].mcs;
		ptr_dbg_ppdu->user[i].preamble = ppdu_desc->user[i].preamble;
		ptr_dbg_ppdu->user[i].gi = ppdu_desc->user[i].gi;
		ptr_dbg_ppdu->user[i].dcm = ppdu_desc->user[i].dcm;
		ptr_dbg_ppdu->user[i].ldpc = ppdu_desc->user[i].ldpc;
		ptr_dbg_ppdu->user[i].delayed_ba =
					ppdu_desc->user[i].delayed_ba;
		ptr_dbg_ppdu->user[i].ack_ba_tlv =
					ppdu_desc->user[i].ack_ba_tlv;
		ptr_dbg_ppdu->user[i].ba_seq_no = ppdu_desc->user[i].ba_seq_no;
		ptr_dbg_ppdu->user[i].start_seq = ppdu_desc->user[i].start_seq;
		qdf_mem_copy(ptr_dbg_ppdu->user[i].ba_bitmap,
			     ppdu_desc->user[i].ba_bitmap,
			     CDP_BA_256_BIT_MAP_SIZE_DWORDS * sizeof(uint32_t));
		qdf_mem_copy(ptr_dbg_ppdu->user[i].enq_bitmap,
			     ppdu_desc->user[i].enq_bitmap,
			     CDP_BA_256_BIT_MAP_SIZE_DWORDS * sizeof(uint32_t));
		qdf_mem_copy(ptr_dbg_ppdu->user[i].failed_bitmap,
			     ppdu_desc->user[i].failed_bitmap,
			     CDP_BA_256_BIT_MAP_SIZE_DWORDS * sizeof(uint32_t));
		ptr_dbg_ppdu->user[i].last_enq_seq =
					ppdu_desc->user[i].last_enq_seq;
		ptr_dbg_ppdu->user[i].ba_size = ppdu_desc->user[i].ba_size;
		ptr_dbg_ppdu->user[i].num_mpdu = ppdu_desc->user[i].num_mpdu;
		ptr_dbg_ppdu->user[i].num_msdu = ppdu_desc->user[i].num_msdu;
		ptr_dbg_ppdu->user[i].is_mcast = ppdu_desc->user[i].is_mcast;
		ptr_dbg_ppdu->user[i].tlv_bitmap =
					ppdu_desc->user[i].tlv_bitmap;
	}
	return ptr_dbg_ppdu;
}

/**
 * debug_ppdu_log_enable_show() - debugfs function to display ppdu_desc
 * @file: qdf debugfs handler
 * @arg: priv data used to get htt_logger_handler
 *
 * Return: QDF_STATUS
 */
static QDF_STATUS debug_ppdu_desc_log_show(qdf_debugfs_file_t file, void *arg)
{
	struct dp_pdev *pdev;
	struct dp_pdev_tx_capture *ptr_tx_cap;
	struct tx_cap_debug_log_info *ptr_log_info;
	struct dbg_tx_comp_ppdu *ptr_dbg_ppdu;
	uint8_t k = 0;
	uint8_t i = 0;
	struct dp_mon_pdev *mon_pdev;

	pdev = (struct dp_pdev *)arg;
	mon_pdev = pdev->monitor_pdev;
	ptr_tx_cap = &mon_pdev->tx_capture;
	ptr_log_info = &ptr_tx_cap->log_info;

	if ((ptr_log_info->stop_seq & (0x1 << PPDU_LOG_DISPLAY_LIST)) &&
	    !ptr_log_info->ppdu_desc_log) {
		ptr_log_info->stop_seq &= ~(0x1 << PPDU_LOG_DISPLAY_LIST);
		return QDF_STATUS_SUCCESS;
	}

	if (!ptr_log_info->ppdu_desc_log) {
		ptr_log_info->pause_dbg_log = 0;
		ptr_log_info->stop_seq |= (0x1 << PPDU_LOG_DISPLAY_LIST);
		return QDF_STATUS_SUCCESS;
	}

	ptr_log_info->pause_dbg_log = 1;

	ptr_dbg_ppdu = ppdu_desc_dbg_dequeue(ptr_log_info);
	if (!ptr_dbg_ppdu) {
		ptr_log_info->pause_dbg_log = 0;
		ptr_log_info->stop_seq |= (0x1 << PPDU_LOG_DISPLAY_LIST);
		return QDF_STATUS_SUCCESS;
	}

	qdf_debugfs_printf(file,
			   "===============================================\n");
	qdf_debugfs_printf(file,
			   "P_ID:%d B_P_ID:%d VDEV:%d N_USR[%d %d] SCH_ID:%d\n",
			   ptr_dbg_ppdu->ppdu_id,
			   ptr_dbg_ppdu->bar_ppdu_id,
			   ptr_dbg_ppdu->vdev_id,
			   ptr_dbg_ppdu->num_users,
			   ptr_dbg_ppdu->bar_num_users,
			   ptr_dbg_ppdu->sched_cmdid);
	qdf_debugfs_printf(file,
			   "TLV:0x%x H_FRM:%d FCTRL:0x%x\n",
			   ptr_dbg_ppdu->tlv_bitmap,
			   ptr_dbg_ppdu->htt_frame_type,
			   ptr_dbg_ppdu->frame_ctrl);
	qdf_debugfs_printf(file,
			   "TSF[%llu - %llu] B_TSF[%llu - %llu]\n",
			   ptr_dbg_ppdu->ppdu_start_timestamp,
			   ptr_dbg_ppdu->ppdu_end_timestamp,
			   ptr_dbg_ppdu->bar_ppdu_start_timestamp,
			   ptr_dbg_ppdu->bar_ppdu_end_timestamp);
	for (k = 0; k < ptr_dbg_ppdu->num_users; k++) {
		struct dbg_tx_comp_ppdu_user *user;

		user = &ptr_dbg_ppdu->user[k];
		qdf_debugfs_printf(file, "USER: %d\n", k);
		qdf_debugfs_printf(file,
				   "\tPEER:%d TID:%d CS:%d TLV:0x%x\n",
				   user->peer_id,
				   user->tid,
				   user->completion_status,
				   user->tlv_bitmap);
		qdf_debugfs_printf(file,
				   "\tFCTRL:0x%x QOS:0x%x\n",
				   user->frame_ctrl,
				   user->qos_ctrl);
		qdf_debugfs_printf(file,
				   "\tMPDU[T:%d S:%d] N_MPDU:%d N_MSDU:%d\n",
				   user->mpdu_tried,
				   user->mpdu_success,
				   user->num_mpdu,
				   user->num_msdu);
		qdf_debugfs_printf(file,
				   "\tMCS:%d NSS:%d PRE:%d BW:%d D_BA:%d\n",
				   user->mcs,
				   user->nss,
				   user->preamble,
				   user->bw,
				   user->delayed_ba);

		qdf_debugfs_printf(file,
				   "\tS_SEQ:%d ENQ_BITMAP[", user->start_seq);
		for (i = 0; i < CDP_BA_256_BIT_MAP_SIZE_DWORDS; i++)
			qdf_debugfs_printf(file, " 0x%x",
					   user->enq_bitmap[i]);
		qdf_debugfs_printf(file, "]\n");

		qdf_debugfs_printf(file,
				   "\tBA_SEQ:%d BA_BITMAP[",
				   user->ba_seq_no);
		for (i = 0; i < CDP_BA_256_BIT_MAP_SIZE_DWORDS; i++)
			qdf_debugfs_printf(file, " 0x%x",
					   user->ba_bitmap[i]);
		qdf_debugfs_printf(file, "]\n");

		qdf_debugfs_printf(file,
				   "\tL_ENQ:%d BA_SZ:%d F_BITMAP[",
				   user->last_enq_seq, user->ba_size);
		for (i = 0; i < CDP_BA_256_BIT_MAP_SIZE_DWORDS; i++)
			qdf_debugfs_printf(file, " 0x%x",
					   user->failed_bitmap[i]);
		qdf_debugfs_printf(file, "]\n");
	}
	qdf_debugfs_printf(file,
			   "===============================================\n");
	/* free the dequeued dbg ppdu_desc */
	qdf_mem_free(ptr_dbg_ppdu);
	return QDF_STATUS_E_AGAIN;
}

/**
 * debug_ppdu_desc_log_write() - write is not allowed in ppdu_desc_log.
 * @priv: file handler to access tx capture log info
 * @buf: received data buffer
 * @len: length of received buffer
 *
 * Return: QDF_STATUS
 */
static QDF_STATUS debug_ppdu_desc_log_write(void *priv,
					    const char *buf,
					    qdf_size_t len)
{
	return -EINVAL;
}

/**
 * debug_ppdu_log_enable_show() - debugfs function to show the
 * enable/disable flag.
 * @file: qdf debugfs handler
 * @arg: priv data used to get htt_logger_handler
 *
 * Return: QDF_STATUS
 */
static QDF_STATUS debug_ppdu_log_enable_show(qdf_debugfs_file_t file, void *arg)
{
	struct dp_pdev *pdev;
	struct dp_pdev_tx_capture *ptr_tx_cap;
	struct tx_cap_debug_log_info *ptr_log_info;
	struct dp_mon_pdev *mon_pdev;

	pdev = (struct dp_pdev *)arg;
	mon_pdev = pdev->monitor_pdev;
	ptr_tx_cap = &mon_pdev->tx_capture;
	ptr_log_info = &ptr_tx_cap->log_info;

	if ((ptr_log_info->stop_seq & (0x1 << PPDU_LOG_ENABLE_LIST))) {
		ptr_log_info->stop_seq &= ~(0x1 << PPDU_LOG_ENABLE_LIST);
		return QDF_STATUS_SUCCESS;
	}

	qdf_debugfs_printf(file, "%u\n", ptr_log_info->ppdu_desc_log);
	ptr_log_info->stop_seq |= (0x1 << PPDU_LOG_ENABLE_LIST);

	return QDF_STATUS_SUCCESS;
}

/**
 * debug_ppdu_log_enable_write() - debugfs function to enable/disable
 * the debugfs flag.
 * @priv: file handler to access tx capture log info
 * @buf: received data buffer
 * @len: length of received buffer
 *
 * Return: QDF_STATUS
 */
static QDF_STATUS debug_ppdu_log_enable_write(void *priv,
					      const char *buf,
					      qdf_size_t len)
{
	struct dp_pdev *pdev;
	struct dp_pdev_tx_capture *ptr_tx_cap;
	struct tx_cap_debug_log_info *ptr_log_info;
	int k, ret;
	struct dp_mon_pdev *mon_pdev;

	pdev = (struct dp_pdev *)priv;
	mon_pdev = pdev->monitor_pdev;
	ptr_tx_cap = &mon_pdev->tx_capture;
	ptr_log_info = &ptr_tx_cap->log_info;

	ret = kstrtoint(buf, 0, &k);
	if ((ret != 0) || ((k != 0) && (k != 1)))
		return QDF_STATUS_E_PERM;

	if (k == 1) {
		/* if ppdu_desc log already 1 return success */
		if (ptr_log_info->ppdu_desc_log)
			return QDF_STATUS_SUCCESS;

		/* initialize ppdu_desc_log to 1 */
		ptr_log_info->ppdu_desc_log = 1;
	} else {
		/* if ppdu_desc log already 0 return success */
		if (!ptr_log_info->ppdu_desc_log)
			return QDF_STATUS_SUCCESS;

		/* initialize ppdu_desc_log to 0 */
		ptr_log_info->ppdu_desc_log = 0;
	}
	return QDF_STATUS_SUCCESS;
}

/*
 * tx_cap_debugfs_log_ppdu_desc: tx capture logging ppdu desc
 * @pdev: DP PDEV handle
 * @nbuf_ppdu: nbuf ppdu
 *
 * return: void
 */
void tx_cap_debugfs_log_ppdu_desc(struct dp_pdev *pdev, qdf_nbuf_t nbuf_ppdu)
{
	struct dp_pdev_tx_capture *ptr_tx_cap;
	struct tx_cap_debug_log_info *ptr_log_info;
	struct dbg_tx_comp_ppdu *ptr_dbg_ppdu;
	struct dbg_tx_comp_ppdu *ptr_tmp_ppdu;
	uint32_t list_size;
	struct dp_mon_pdev *mon_pdev = pdev->monitor_pdev;

	ptr_tx_cap = &mon_pdev->tx_capture;
	ptr_log_info = &ptr_tx_cap->log_info;

	if (!ptr_log_info->ppdu_desc_log || ptr_log_info->pause_dbg_log)
		return;

	ptr_dbg_ppdu = dbg_copy_ppdu_desc(nbuf_ppdu);
	if (!ptr_dbg_ppdu)
		return;

	list_size = ppdu_desc_dbg_enqueue(ptr_log_info, ptr_dbg_ppdu);
	dp_tx_capture_info("%pK: enqueue list_size:%d\n", pdev->soc, list_size);

	if (list_size >= ptr_log_info->ppdu_queue_size) {
		ptr_tmp_ppdu = ppdu_desc_dbg_dequeue(ptr_log_info);
		qdf_mem_free(ptr_tmp_ppdu);
	}
}

#define DEBUGFS_FOPS(func_base)                                 \
	{                                                       \
		.name = #func_base,                             \
		.ops = {                                        \
			.show = debug_##func_base##_show,       \
			.write = debug_##func_base##_write,     \
			.priv = NULL,                           \
		}                                               \
	}

struct tx_cap_debugfs_info tx_cap_debugfs_infos[NUM_TX_CAP_DEBUG_INFOS] = {
	DEBUGFS_FOPS(ppdu_log_enable),
	DEBUGFS_FOPS(ppdu_desc_log),
};

/*
 * tx_cap_debugfs_remove: remove dentry and file created.
 * @ptr_log_info: tx capture debugfs log structure
 *
 * return: void
 */
static void tx_cap_debugfs_remove(struct tx_cap_debug_log_info *ptr_log_info)
{
	qdf_dentry_t dentry;
	uint8_t i = 0;

	for (i = 0; i < NUM_TX_CAP_DEBUG_INFOS; ++i) {
		dentry = ptr_log_info->debugfs_de[i];
		if (dentry) {
			qdf_debugfs_remove_file(dentry);
			ptr_log_info->debugfs_de[i] = NULL;
		}
	}

	dentry = ptr_log_info->tx_cap_debugfs_dir;
	if (dentry) {
		qdf_debugfs_remove_dir(dentry);
		ptr_log_info->tx_cap_debugfs_dir = NULL;
	}
}

/*
 * dp_tx_capture_debugfs_init_1_0: tx capture debugfs init
 * @pdev: DP PDEV handle
 *
 * return: QDF_STATUS
 */
QDF_STATUS dp_tx_capture_debugfs_init_1_0(struct dp_pdev *pdev)
{
	qdf_dentry_t dentry;
	struct dp_soc *soc;
	struct dp_pdev_tx_capture *ptr_tx_cap;
	struct tx_cap_debug_log_info *ptr_log_info;
	struct tx_cap_debugfs_info *ptr_debugfs_info;
	uint8_t i = 0;
	char buf[32];
	char *name = NULL;
	struct dp_mon_pdev *mon_pdev = pdev->monitor_pdev;

	soc = pdev->soc;
	ptr_tx_cap = &mon_pdev->tx_capture;
	ptr_log_info = &ptr_tx_cap->log_info;

	if (soc->cdp_soc.ol_ops->get_device_name) {
		name = soc->cdp_soc.ol_ops->get_device_name(
				pdev->soc->ctrl_psoc, pdev->pdev_id);
	}

	if (!name)
		return QDF_STATUS_E_FAILURE;

	/* directory creation with name */
	snprintf(buf, sizeof(buf), "tx_cap_log_%s", name);
	dentry = qdf_debugfs_create_dir(buf, NULL);

	if (!dentry) {
		dp_tx_capture_err("%pK: error while creating debugfs dir for %s", pdev->soc, buf);
		goto out;
	}

	ptr_log_info->tx_cap_debugfs_dir = dentry;

	qdf_mem_copy(ptr_log_info->tx_cap_debugfs_infos,
		     tx_cap_debugfs_infos,
		     (sizeof(struct tx_cap_debugfs_info) *
		      NUM_TX_CAP_DEBUG_INFOS));

	for (i = 0; i < NUM_TX_CAP_DEBUG_INFOS; i++) {
		ptr_debugfs_info = &ptr_log_info->tx_cap_debugfs_infos[i];
		/* store pdev tx capture pointer to private data pointer */
		ptr_debugfs_info->ops.priv = pdev;

		ptr_log_info->debugfs_de[i] = qdf_debugfs_create_file(
				ptr_debugfs_info->name,
				TX_CAP_DBG_FILE_PERM,
				ptr_log_info->tx_cap_debugfs_dir,
				&ptr_debugfs_info->ops);

		if (!ptr_log_info->debugfs_de[i]) {
			dp_tx_capture_err("%pK: debug Entry creation failed[%s]!",
					  pdev->soc, ptr_debugfs_info->name);
			goto out;
		}
	}

	return QDF_STATUS_SUCCESS;
out:
	tx_cap_debugfs_remove(ptr_log_info);
	return QDF_STATUS_E_FAILURE;
}

/*
 * dp_tx_capture_debugfs_deinit_1_0: tx capture debugfs deinit
 * @pdev: DP PDEV handle
 *
 * return: void
 */
void dp_tx_capture_debugfs_deinit_1_0(struct dp_pdev *pdev)
{
	struct dp_pdev_tx_capture *ptr_tx_cap;
	struct tx_cap_debug_log_info *ptr_log_info;
	struct dp_mon_pdev *mon_pdev = pdev->monitor_pdev;

	ptr_tx_cap = &mon_pdev->tx_capture;
	ptr_log_info = &ptr_tx_cap->log_info;

	tx_cap_debugfs_remove(ptr_log_info);
}

#ifdef WLAN_TX_PKT_CAPTURE_ENH_DEBUG
static void
dp_get_peer_tx_capture_debug_stats(struct cdp_peer_tx_capture_stats *stats,
				   struct dp_peer_tx_capture *tx_capture)
{
	struct dp_peer_tx_capture_stats *tx_cap_stats = &tx_capture->stats;

	stats->msdu[PEER_MSDU_SUCC] = tx_cap_stats->msdu[PEER_MSDU_SUCC];
	stats->msdu[PEER_MSDU_ENQ] = tx_cap_stats->msdu[PEER_MSDU_ENQ];
	stats->msdu[PEER_MSDU_DEQ] = tx_cap_stats->msdu[PEER_MSDU_DEQ];
	stats->msdu[PEER_MSDU_FLUSH] = tx_cap_stats->msdu[PEER_MSDU_FLUSH];
	stats->msdu[PEER_MSDU_DROP] = tx_cap_stats->msdu[PEER_MSDU_DROP];
	stats->msdu[PEER_MSDU_XRETRY] = tx_cap_stats->msdu[PEER_MSDU_XRETRY];
	stats->mpdu[PEER_MPDU_TRI] = tx_cap_stats->mpdu[PEER_MPDU_TRI];
	stats->mpdu[PEER_MPDU_SUCC] = tx_cap_stats->mpdu[PEER_MPDU_SUCC];
	stats->mpdu[PEER_MPDU_RESTITCH] =
					tx_cap_stats->mpdu[PEER_MPDU_RESTITCH];
	stats->mpdu[PEER_MPDU_ARR] = tx_cap_stats->mpdu[PEER_MPDU_ARR];
	stats->mpdu[PEER_MPDU_CLONE] = tx_cap_stats->mpdu[PEER_MPDU_CLONE];
	stats->mpdu[PEER_MPDU_TO_STACK] =
					tx_cap_stats->mpdu[PEER_MPDU_TO_STACK];
}
#else /* WLAN_TX_PKT_CAPTURE_ENH_DEBUG */
static void
dp_get_peer_tx_capture_debug_stats(struct cdp_peer_tx_capture_stats *stats,
				   struct dp_peer_tx_capture *tx_capture)
{
}
#endif /* WLAN_TX_PKT_CAPTURE_ENH_DEBUG */

QDF_STATUS
dp_get_peer_tx_capture_stats(struct dp_peer *peer,
			     struct cdp_peer_tx_capture_stats *stats)
{
	struct dp_peer_tx_capture *tx_capture;
	int tid;

	if (!peer || !peer->monitor_peer)
		return QDF_STATUS_E_FAILURE;

	tx_capture = &peer->monitor_peer->tx_capture;
	dp_get_peer_tx_capture_debug_stats(stats, tx_capture);

	for (tid = 0; tid < DP_MAX_TIDS; tid++) {
		stats->len_stats[tid].defer_msdu_len =
		qdf_nbuf_queue_len(&tx_capture->tx_tid[tid].defer_msdu_q);
		stats->len_stats[tid].tasklet_msdu_len =
		qdf_nbuf_queue_len(&tx_capture->tx_tid[tid].msdu_comp_q);
		stats->len_stats[tid].pending_q_len =
		qdf_nbuf_queue_len(&tx_capture->tx_tid[tid].pending_ppdu_q);
	}

	return QDF_STATUS_SUCCESS;
}

static void dp_peer_consolidate_tid_qlen(struct dp_soc *soc,
					 struct dp_peer *peer,
					 void *arg)
{
	int tid;
	struct dp_tx_tid *tx_tid;
	struct cdp_tid_q_len *tid_q_len = (struct cdp_tid_q_len *)arg;

	if (!tid_q_len || !peer || !peer->monitor_peer)
		return;

	for (tid = 0; tid < DP_MAX_TIDS; tid++) {
		tx_tid = &peer->monitor_peer->tx_capture.tx_tid[tid];
		tid_q_len->defer_msdu_len +=
				qdf_nbuf_queue_len(&tx_tid->defer_msdu_q);
		tid_q_len->tasklet_msdu_len +=
				qdf_nbuf_queue_len(&tx_tid->msdu_comp_q);
		tid_q_len->pending_q_len +=
				qdf_nbuf_queue_len(&tx_tid->pending_ppdu_q);
	}
}

QDF_STATUS
dp_get_pdev_tx_capture_stats(struct dp_pdev *pdev,
			     struct cdp_pdev_tx_capture_stats *stats)
{
	struct dp_pdev_tx_capture *ptr_tx_cap;
	uint8_t i = 0, j = 0;
	uint32_t ppdu_stats_ms = 0;
	uint32_t now_ms = 0;
	struct dp_mon_pdev *mon_pdev;

	if (!pdev)
		return QDF_STATUS_E_FAILURE;

	mon_pdev = pdev->monitor_pdev;
	if (!mon_pdev)
		return QDF_STATUS_E_FAILURE;

	ptr_tx_cap = &mon_pdev->tx_capture;
	ppdu_stats_ms = ptr_tx_cap->ppdu_stats_ms;
	now_ms = qdf_system_ticks_to_msecs(qdf_system_ticks());

	stats->last_rcv_ppdu = now_ms - ppdu_stats_ms;
	stats->ppdu_stats_queue_depth = ptr_tx_cap->ppdu_stats_queue_depth;
	stats->ppdu_stats_defer_queue_depth =
				ptr_tx_cap->ppdu_stats_defer_queue_depth;
	stats->ppdu_dropped = ptr_tx_cap->ppdu_dropped;
	stats->pend_ppdu_dropped = ptr_tx_cap->pend_ppdu_dropped;
	stats->peer_mismatch = ptr_tx_cap->peer_mismatch;
	stats->ppdu_flush_count = ptr_tx_cap->ppdu_flush_count;
	stats->msdu_threshold_drop = ptr_tx_cap->msdu_threshold_drop;
	for (i = 0; i < CDP_TXCAP_MAX_TYPE; i++) {
		for (j = 0; j < CDP_TXCAP_MAX_SUBTYPE; j++) {
			if (ptr_tx_cap->ctl_mgmt_q[i][j].qlen)
				stats->ctl_mgmt_q_len[i][j] =
					ptr_tx_cap->ctl_mgmt_q[i][j].qlen;
			if (ptr_tx_cap->retries_ctl_mgmt_q[i][j].qlen)
				stats->retries_ctl_mgmt_q_len[i][j] =
				ptr_tx_cap->retries_ctl_mgmt_q[i][j].qlen;
		}
	}
	for (i = 0; i < qdf_min((uint32_t)TX_CAP_HTT_MAX_FTYPE,
				(uint32_t)CDP_TX_CAP_HTT_MAX_FTYPE); i++)
		stats->htt_frame_type[i] = ptr_tx_cap->htt_frame_type[i];

	dp_pdev_iterate_peer(pdev, dp_peer_consolidate_tid_qlen,
			     &stats->len_stats, DP_MOD_ID_TX_CAPTURE);

	return QDF_STATUS_SUCCESS;
}
#endif
