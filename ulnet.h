#include "sam2.h"

#include "juice/juice.h"
#include "zstd.h"
#include "common/xxhash.h"
#include "fec.h"

#include <stdint.h>
#include <assert.h>

#ifndef ULNET_LINKAGE
#ifdef __cplusplus
#define ULNET_LINKAGE extern "C"
#else
#define ULNET_LINKAGE extern
#endif
#endif

// The payload here is regarding the max payload that we *can* use
// We don't want to exceed the MTU because that can result in guranteed lost packets under certain conditions
// Considering various things like UDP/IP headers, STUN/TURN headers, and additional junk 
// load-balancers/routers might add I keep this conservative
#define ULNET_PACKET_SIZE_BYTES_MAX 1408

#define ULNET_SPECTATOR_MAX 55
#define ULNET_CORE_OPTIONS_MAX 128
#define ULNET_STATE_PACKET_HISTORY_SIZE 256

#define ULNET_FLAGS_MASK                      0x0F
#define ULNET_CHANNEL_MASK                    0xF0

#define ULNET_CHANNEL_EXTRA                   0x00
#define ULNET_CHANNEL_INPUT                   0x10
#define ULNET_CHANNEL_INPUT_AUDIT_CONSISTENCY 0x20
#define ULNET_CHANNEL_SAVESTATE_TRANSFER      0x30
#define ULNET_CHANNEL_DESYNC_DEBUG            0xF0

#define ULNET_WAITING_FOR_SAVE_STATE_SENTINEL INT64_MAX

#define ULNET_SESSION_FLAG_TICKED                 0b00000001ULL
#define ULNET_SESSION_FLAG_CORE_OPTIONS_DIRTY     0b00000010ULL

// @todo Remove this define once it becomes possible through normal featureset
#define ULNET__DEBUG_EVERYONE_ON_PORT_0

// This constant defines the maximum number of frames that can be buffered before blocking.
// A value of 2 implies no delay can be accomidated.
//```
// Consider the following scenario:
// logical-time | peer a        | peer b
// --------------------------------------------
// 0            | send input 0a | send input 0b
// 1            | recv input 0b | recv input 0a
// 2            | ------------- | tick frame 0
// 3            | ------------- | send input 1b
// 4            | recv input 1b | -------------
// 5            | tick frame 0  | -------------
//```
// The issue occurs at logical-time 4 when peer a receives input 1 before ticking frame 0.
// If the input buffer only holds 1 frame, the input packet for frame 0 would be overwritten.
// To handle the case where a peer immediately ticks and sends an input after receiving,
// the input buffer needs to hold at least 2 frames.
//
// Setting ULNET_DELAY_BUFFER_SIZE to 2 allows for no frame delay while still handling this scenario.
// The following constant is set to 8 which yields 3 frames of delay this corresponds to a max RTT PING of 100 ms to not stutter
#define ULNET_DELAY_BUFFER_SIZE 8

#define ULNET_DELAY_FRAMES_MAX (ULNET_DELAY_BUFFER_SIZE/2-1)

#define ULNET_PORT_COUNT 8
typedef int16_t ulnet_input_state_t[64]; // This must be a POD for putting into packets

typedef struct ulnet_core_option {
    char key[128];
    char value[128];
} ulnet_core_option_t;

// @todo This is really sparse so you should just add routines to read values from it in the serialized format
typedef struct {
    int64_t frame;
    ulnet_input_state_t input_state[ULNET_DELAY_BUFFER_SIZE][ULNET_PORT_COUNT];
    sam2_room_t room_xor_delta[ULNET_DELAY_BUFFER_SIZE];
    ulnet_core_option_t core_option[ULNET_DELAY_BUFFER_SIZE]; // Max 1 option per frame provided by the authority
} ulnet_state_t;
SAM2_STATIC_ASSERT(
    sizeof(ulnet_state_t) ==
    (sizeof(((ulnet_state_t *)0)->frame)
    + sizeof(((ulnet_state_t *)0)->input_state)
    + sizeof(((ulnet_state_t *)0)->room_xor_delta)
    + sizeof(((ulnet_state_t *)0)->core_option)),
    "ulnet_state_t is not packed"
);

typedef struct {
    uint8_t channel_and_port;
    uint8_t coded_state[];
} ulnet_state_packet_t;

// @todo Just roll this all into ulnet_state_t
typedef struct {
    uint8_t channel_and_flags;
    uint8_t spacing[7];

    int64_t frame;
    int64_t save_state_hash[ULNET_DELAY_BUFFER_SIZE];
    int64_t input_state_hash[ULNET_DELAY_BUFFER_SIZE];
    //int64_t options_state_hash[ULNET_DELAY_BUFFER_SIZE]; // @todo
} desync_debug_packet_t;

#define FEC_PACKET_GROUPS_MAX 16
#define FEC_REDUNDANT_BLOCKS 16 // ULNET is hardcoded based on this value so it can't really be changed

#define ULNET_SAVESTATE_TRANSFER_FLAG_K_IS_239         0b0001
#define ULNET_SAVESTATE_TRANSFER_FLAG_SEQUENCE_HI_IS_0 0b0010

// @todo Just get rid of these
#define COMPRESSED_SAVE_STATE_BOUND_BYTES ZSTD_COMPRESSBOUND(20 * 1024 * 1024) // @todo Magic number
#define COMPRESSED_CORE_OPTIONS_BOUND_BYTES ZSTD_COMPRESSBOUND(sizeof(ulnet_core_option_t[ULNET_CORE_OPTIONS_MAX])) // @todo Probably make the type in here a typedef
#define COMPRESSED_DATA_WITH_REDUNDANCY_BOUND_BYTES (255 * (COMPRESSED_SAVE_STATE_BOUND_BYTES + COMPRESSED_CORE_OPTIONS_BOUND_BYTES) / (255 - FEC_REDUNDANT_BLOCKS))

typedef struct {
    uint8_t channel_and_flags;
    union {
        uint8_t reed_solomon_k;
        uint8_t packet_groups;
        uint8_t sequence_hi;
    };

    uint8_t sequence_lo;

    //uint8_t payload[]; // Variable size; at most ULNET_PACKET_SIZE_BYTES_MAX-3
} ulnet_save_state_packet_header_t;

typedef struct {
    uint8_t channel_and_flags;
    union {
        uint8_t reed_solomon_k;
        uint8_t packet_groups;
        uint8_t sequence_hi;
    };

    uint8_t sequence_lo;

    uint8_t payload[ULNET_PACKET_SIZE_BYTES_MAX-3]; // Variable size; at most ULNET_PACKET_SIZE_BYTES_MAX-3
} ulnet_save_state_packet_fragment2_t;
SAM2_STATIC_ASSERT(sizeof(ulnet_save_state_packet_fragment2_t) == ULNET_PACKET_SIZE_BYTES_MAX, "Savestate transfer is the wrong size");

typedef struct {
    int64_t total_size_bytes; // @todo This isn't necessary
    int64_t frame_counter;
    sam2_room_t room;
    uint64_t encoding_chain; // @todo probably won't use this
    uint64_t xxhash;

    int64_t compressed_options_size;
    int64_t compressed_savestate_size;
    int64_t decompressed_savestate_size;
#if 0
    uint8_t compressed_savestate_data[compressed_savestate_size];
    uint8_t compressed_options_data[compressed_options_size];
#else
    uint8_t compressed_data[]; 
#endif
} savestate_transfer_payload_t;

typedef struct ulnet_session {
    int64_t frame_counter;
    int64_t delay_frames;
    int64_t core_wants_tick_at_unix_usec;
    int64_t flags;
    uint64_t our_peer_id;

    sam2_room_t room_we_are_in;
    uint64_t spectator_peer_ids[ULNET_SPECTATOR_MAX];
    sam2_room_t next_room_xor_delta;

    ulnet_core_option_t core_options[ULNET_CORE_OPTIONS_MAX]; // @todo I don't like this here

    // @todo Change these so they're all peer_*
    juice_agent_t *agent               [SAM2_PORT_MAX + 1 /* Plus Authority */ + ULNET_SPECTATOR_MAX];
    int64_t        peer_desynced_frame [SAM2_PORT_MAX + 1 /* Plus Authority */ + ULNET_SPECTATOR_MAX];
    ulnet_state_t  state               [SAM2_PORT_MAX + 1 /* Plus Authority */];
    unsigned char  state_packet_history[SAM2_PORT_MAX + 1 /* Plus Authority */][ULNET_STATE_PACKET_HISTORY_SIZE][ULNET_PACKET_SIZE_BYTES_MAX];
    uint64_t       peer_needs_sync_bitfield;

    int64_t spectator_count;

    desync_debug_packet_t desync_debug_packet;

    int zstd_compress_level;
    unsigned char remote_savestate_transfer_packets[COMPRESSED_DATA_WITH_REDUNDANCY_BOUND_BYTES + FEC_PACKET_GROUPS_MAX * (GF_SIZE - FEC_REDUNDANT_BLOCKS) * sizeof(ulnet_save_state_packet_header_t)];
    int64_t remote_savestate_transfer_offset;
    uint8_t remote_packet_groups; // This is used to bookkeep how much data we actually need to receive to reform the complete savestate
    void *fec_packet[FEC_PACKET_GROUPS_MAX][GF_SIZE - FEC_REDUNDANT_BLOCKS];
    int fec_index[FEC_PACKET_GROUPS_MAX][GF_SIZE - FEC_REDUNDANT_BLOCKS];
    int fec_index_counter[FEC_PACKET_GROUPS_MAX]; // Counts packets received in each "packet group"

    void *user_ptr;
    int (*sam2_send_callback)(void *user_ptr, char *response);
    int (*populate_core_options_callback)(void *user_ptr, ulnet_core_option_t options[ULNET_CORE_OPTIONS_MAX]);

    bool (*retro_unserialize)(const void *data, size_t size);
} ulnet_session_t;

ULNET_LINKAGE void ulnet_send_save_state(ulnet_session_t *session, juice_agent_t *agent, void *save_state, size_t save_state_size, int64_t save_state_frame);
ULNET_LINKAGE int ulnet_startup_ice_for_peer(ulnet_session_t *session, uint64_t peer_id, const char *remote_description);
ULNET_LINKAGE void ulnet_disconnect_peer(ulnet_session_t *session, int peer_port);
ULNET_LINKAGE void ulnet_move_peer(ulnet_session_t *session, int peer_existing_port, int peer_new_port);
ULNET_LINKAGE void ulnet_session_init_defaulted(ulnet_session_t *session);

static inline int ulnet_our_port(ulnet_session_t *session) {
    // @todo There is a bug here where we are sending out packets as the authority when we are not the authority
    if (session->room_we_are_in.flags & SAM2_FLAG_ROOM_IS_NETWORK_HOSTED) {
        int port = sam2_get_port_of_peer(&session->room_we_are_in, session->our_peer_id);
        assert(port != -1);

        return port;
    } else {
        return SAM2_AUTHORITY_INDEX;
    }
}

static inline int ulnet_locate_peer(ulnet_session_t *session, uint64_t peer_id) {
    int room_port, spectator_port;
    SAM2_LOCATE(session->room_we_are_in.peer_ids, peer_id, room_port);
    SAM2_LOCATE(session->spectator_peer_ids,      peer_id, spectator_port);

    return spectator_port != -1 ? spectator_port + SAM2_PORT_MAX+1 : room_port;
}

static bool ulnet_is_authority(ulnet_session_t *session) {
    return    session->our_peer_id == session->room_we_are_in.peer_ids[SAM2_AUTHORITY_INDEX]
           || session->room_we_are_in.peer_ids[SAM2_AUTHORITY_INDEX] == 0; // @todo I don't think this extra check should be necessary
}

static bool ulnet_is_spectator(ulnet_session_t *session, uint64_t peer_id) {
    return    session->room_we_are_in.flags & SAM2_FLAG_ROOM_IS_NETWORK_HOSTED
           && sam2_get_port_of_peer(&session->room_we_are_in, peer_id) == -1;
}

static inline void ulnet__xor_delta(void *dest, void *src, int size) {
    for (int i = 0; i < size; i++) {
        ((uint8_t *) dest)[i] ^= ((uint8_t *) src)[i];
    }
}

#if defined(ULNET_IMPLEMENTATION)
#if defined(ULNET_IMGUI)
#include "imgui.h"
#include "implot.h"
#define IMH(statement) statement
#else
#define IMH(statement)
#endif

static void ulnet__logical_partition(int sz, int redundant, int *n, int *out_k, int *packet_size, int *packet_groups) {
    int k_max = GF_SIZE - redundant;
    *packet_groups = 1;
    int k = (sz - 1) / (*packet_groups * *packet_size) + 1;

    if (k > k_max) {
        *packet_groups = (k - 1) / k_max + 1;
        *packet_size = (sz - 1) / (k_max * *packet_groups) + 1;
        k = (sz - 1) / (*packet_groups * *packet_size) + 1;
    }

    *n = k + k * redundant / k_max;
    *out_k = k;
}

// This is a little confusing since the lower byte of sequence corresponds to the largest stride
static int64_t ulnet__logical_partition_offset_bytes(uint8_t sequence_hi, uint8_t sequence_lo, int block_size_bytes, int block_stride) {
    return (int64_t) sequence_hi * block_size_bytes + sequence_lo * block_size_bytes * block_stride;
}

ULNET_LINKAGE void ulnet_input_poll(ulnet_session_t *session, ulnet_input_state_t (*input_state)[ULNET_PORT_COUNT]) {
    for (int peer_idx = 0; peer_idx < SAM2_PORT_MAX+1; peer_idx++) {
        if (   session->room_we_are_in.peer_ids[peer_idx] > SAM2_PORT_SENTINELS_MAX
            || peer_idx == SAM2_AUTHORITY_INDEX
            && !(session->room_we_are_in.flags & (SAM2_FLAG_PORT0_PEER_IS_INACTIVE << peer_idx))) {

            if (!(session->room_we_are_in.flags & SAM2_FLAG_ROOM_IS_NETWORK_HOSTED)) {
                assert(peer_idx == SAM2_AUTHORITY_INDEX);
            }

            assert(session->state[peer_idx].frame <= session->frame_counter + (ULNET_DELAY_BUFFER_SIZE-1));
            assert(session->state[peer_idx].frame >= session->frame_counter);
            for (int i = 0; i < SAM2_ARRAY_LENGTH((*input_state)[0]); i++) {
                #if defined(ULNET__DEBUG_EVERYONE_ON_PORT_0)
                int port = 0;
                #else
                int port = peer_idx;
                #endif
                (*input_state)[port][i] |= session->state[peer_idx].input_state[session->frame_counter % ULNET_DELAY_BUFFER_SIZE][port][i];
            }
        }
    }
}

// @todo Weird interface
ULNET_LINKAGE ulnet_input_state_t (*ulnet_query_generate_next_input(ulnet_session_t *session, ulnet_core_option_t *next_frame_option))[ULNET_PORT_COUNT] {
    // Poll input with buffering for netplay
    if (!ulnet_is_spectator(session, session->our_peer_id) && session->state[ulnet_our_port(session)].frame < session->frame_counter + session->delay_frames) {
        // @todo The preincrement does not make sense to me here, but things have been working
        int64_t next_buffer_index = ++session->state[ulnet_our_port(session)].frame % ULNET_DELAY_BUFFER_SIZE;

        session->state[ulnet_our_port(session)].core_option[next_buffer_index] = *next_frame_option;
        memset(next_frame_option, 0, sizeof(*next_frame_option));

        //if (ulnet_is_authority(session)) {
            session->state[ulnet_our_port(session)].room_xor_delta[next_buffer_index] = session->next_room_xor_delta;
            memset(&session->next_room_xor_delta, 0, sizeof(session->next_room_xor_delta));
        //}

        return &session->state[ulnet_our_port(session)].input_state[next_buffer_index];
    }

    return NULL;
}

#ifdef _WIN32
int64_t get_unix_time_microseconds() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);

    ULARGE_INTEGER ul;
    ul.LowPart = ft.dwLowDateTime;
    ul.HighPart = ft.dwHighDateTime;

    int64_t unix_time = (int64_t)(ul.QuadPart - 116444736000000000LL) / 10;

    return unix_time;
}
#else
int64_t get_unix_time_microseconds() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000000 + tv.tv_usec;
}
#endif

double core_wants_tick_in_seconds(int64_t core_wants_tick_at_unix_usec) {
    double seconds = (core_wants_tick_at_unix_usec - get_unix_time_microseconds()) / 1000000.0;
    return seconds;
}

#define ULNET_POLL_SESSION_SAVED_STATE 0b00000001
#define ULNET_POLL_SESSION_TICKED      0b00000010
// This procedure always sends an input packet if the core is ready to tick. This subsumes retransmission logic and generally makes protocol logic less strict
ULNET_LINKAGE int ulnet_poll_session(ulnet_session_t *session, bool force_save_state_on_tick, uint8_t *save_state, size_t save_state_size, double frame_rate,
    void (*retro_run)(void), bool (*retro_serialize)(void *, size_t), bool (*retro_unserialize)(const void *, size_t)) {
    IMH(ImGui::Begin("P2P UDP Netplay", NULL, ImGuiWindowFlags_AlwaysAutoResize);)
    int status = 0;

    session->retro_unserialize = retro_unserialize; // If used this is invoked through a callback within this function call
    if (   !ulnet_is_spectator(session, session->our_peer_id) 
        && session->room_we_are_in.flags & SAM2_FLAG_ROOM_IS_NETWORK_HOSTED) {
        uint8_t _[RLE8_ENCODE_UPPER_BOUND(ULNET_PACKET_SIZE_BYTES_MAX)];
        ulnet_state_packet_t *input_packet = (ulnet_state_packet_t *) _;
        input_packet->channel_and_port = ULNET_CHANNEL_INPUT | ulnet_our_port(session);
        int64_t actual_payload_size = rle8_encode(
            (uint8_t *)&session->state[ulnet_our_port(session)],
            sizeof(session->state[0]),
            input_packet->coded_state
        );

        void *next_history_packet = &session->state_packet_history[ulnet_our_port(session)][session->state[ulnet_our_port(session)].frame % ULNET_STATE_PACKET_HISTORY_SIZE];
        memset(next_history_packet, 0, sizeof(session->state_packet_history[0][0]));
        memcpy(
            next_history_packet,
            input_packet,
            actual_payload_size
        );

        if (sizeof(ulnet_state_packet_t) + actual_payload_size > ULNET_PACKET_SIZE_BYTES_MAX) {
            SAM2_LOG_FATAL("Input packet too large to send");
        }

        for (int p = 0; p < SAM2_ARRAY_LENGTH(session->agent); p++) {
            if (!session->agent[p]) continue;
            juice_state_t state = juice_get_state(session->agent[p]);

            // Wait until we can send netplay messages to everyone without fail
            if (   state == JUICE_STATE_CONNECTED || state == JUICE_STATE_COMPLETED
                && !ulnet_is_spectator(session, session->our_peer_id)) {
                juice_send(session->agent[p], (const char *) input_packet, sizeof(ulnet_state_packet_t) + actual_payload_size);
                SAM2_LOG_DEBUG("Sent input packet for frame %" PRId64 " dest peer_ids[%d]=%" PRIx64,
                    session->state[SAM2_AUTHORITY_INDEX].frame, p, session->room_we_are_in.peer_ids[p]);
            }
        }
    }

#if defined(ULNET_IMGUI)
    { // Plot Input Packet Size vs. Frame
        // @todo The gaps in the graph can be explained by out-of-order arrival of packets I think I don't even record those to history but I should
        //       There is some other weird behavior that might be related to not checking the frame field in the packet if its too old it shouldn't be in the plot obviously
        ImPlot::SetNextAxisLimits(ImAxis_X1, session->frame_counter - g_sample_size, session->frame_counter, ImGuiCond_Always);
        ImPlot::SetNextAxisLimits(ImAxis_Y1, 0.0f, 512, ImGuiCond_Always);
        if (   session->room_we_are_in.flags & SAM2_FLAG_ROOM_IS_NETWORK_HOSTED
            && ImPlot::BeginPlot("State-Packet Size vs. Frame")) {
            ImPlot::SetupAxis(ImAxis_X1, "ulnet_state_t::frame");
            ImPlot::SetupAxis(ImAxis_Y1, "Size Bytes");
            for (int p = 0; p < SAM2_PORT_MAX+1; p++) {
                if (session->room_we_are_in.peer_ids[p] <= SAM2_PORT_SENTINELS_MAX) continue;
                static int input_packet_size[SAM2_PORT_MAX+1][MAX_SAMPLE_SIZE] = {0};

                uint8_t *peer_packet = session->state_packet_history[p][session->frame_counter % ULNET_STATE_PACKET_HISTORY_SIZE];
                int packet_size_bytes = 0;
                uint16_t u16_0 = 0;
                for (; packet_size_bytes < ULNET_PACKET_SIZE_BYTES_MAX; packet_size_bytes++) {
                    if (memcmp(peer_packet + packet_size_bytes, &u16_0, sizeof(u16_0)) == 0) break;
                }
                input_packet_size[p][session->frame_counter % g_sample_size] = packet_size_bytes;

                char label[32] = {0};
                if (p == SAM2_AUTHORITY_INDEX) {
                    strcpy(label, "Authority");
                } else {
                    sprintf(label, "Port %d", p);
                }

                int xs[MAX_SAMPLE_SIZE];
                int ys[MAX_SAMPLE_SIZE];
                for (int frame = SAM2_MAX(0, session->frame_counter - g_sample_size + 1), j = 0; j < g_sample_size; frame++, j++) {
                    xs[j] = frame;
                    ys[j] = input_packet_size[p][frame % g_sample_size];
                }

                ImPlot::PlotLine(label, xs, ys, g_sample_size);
            }

            ImPlot::EndPlot();
        }
    }
#endif

    // We need to poll agents to make progress on the ICE connection
    juice_agent_t *agent[SAM2_ARRAY_LENGTH(session->agent)] = {0};
    int agent_count = 0;
    for (int p = 0; p < SAM2_ARRAY_LENGTH(session->agent); p++) {
        if (session->agent[p]) {
            agent[agent_count++] = session->agent[p];
        }
    }

    int timeout_milliseconds = 1e3 * core_wants_tick_in_seconds(session->core_wants_tick_at_unix_usec);
    timeout_milliseconds = SAM2_MAX(0, timeout_milliseconds);

    int ret = juice_user_poll(agent, agent_count, timeout_milliseconds);
    // This will call ulnet_receive_packet_callback in a loop
    if (ret < 0) {
        SAM2_LOG_FATAL("Error polling agent (%d)\n", ret);
    }

    // Reconstruct input required for next tick if we're spectating... this crashes when without sufficient history to pull from @todo
    if (ulnet_is_spectator(session, session->our_peer_id)) {
        for (int p = 0; p < SAM2_PORT_MAX+1; p++) {
            if (session->room_we_are_in.peer_ids[p] <= SAM2_PORT_SENTINELS_MAX) continue;

            int i;
            for (i = ULNET_DELAY_BUFFER_SIZE-1; i >= 0; i--) {
                int64_t frame = -1;
                ulnet_state_packet_t *ulnet_state_packet_that_could_contain_input_for_current_frame = (ulnet_state_packet_t *) session->state_packet_history[p][(session->frame_counter + i) % ULNET_STATE_PACKET_HISTORY_SIZE];
                rle8_decode(ulnet_state_packet_that_could_contain_input_for_current_frame->coded_state, ULNET_PACKET_SIZE_BYTES_MAX, (uint8_t *) &frame, sizeof(frame));
                if (SAM2_ABS(frame - session->frame_counter) < ULNET_DELAY_BUFFER_SIZE) {
                    int64_t input_consumed = 0;
                    int64_t decode_size = rle8_decode_extra(ulnet_state_packet_that_could_contain_input_for_current_frame->coded_state, ULNET_PACKET_SIZE_BYTES_MAX,
                        &input_consumed, (uint8_t *) &session->state[p], sizeof(session->state[p]));

//                        SAM2_LOG_DEBUG("Reconstructed input for frame %" PRId64 " from peer %" PRIx64 "consumed %" PRId64 " bytes of input to produce %" PRId64,
//                            session->frame_counter, session->room_we_are_in.peer_ids[p], input_consumed, decode_size);

                    break;
                }
            }

            //if (i == ULNET_DELAY_BUFFER_SIZE) {
            //    SAM2_LOG_FATAL("Failed to reconstruct input for frame %" PRId64 " from peer %" PRIx64 "\n", session->frame_counter, session->room_we_are_in.peer_ids[p]);
            //}
        }
    }

IMH(ImGui::SeparatorText("Things We are Waiting on Before we can Tick");)
IMH(if                            (session->frame_counter == ULNET_WAITING_FOR_SAVE_STATE_SENTINEL) { ImGui::Text("Waiting for savestate"); })
    bool netplay_ready_to_tick = !(session->frame_counter == ULNET_WAITING_FOR_SAVE_STATE_SENTINEL);
    if (session->room_we_are_in.flags & SAM2_FLAG_ROOM_IS_NETWORK_HOSTED) {
        for (int p = 0; p < SAM2_PORT_MAX+1; p++) {
            if (session->room_we_are_in.peer_ids[p] <= SAM2_PORT_SENTINELS_MAX) continue;
        IMH(if                      (session->state[p].frame <  session->frame_counter) { ImGui::Text("Input state on port %d is too old", p); })
            netplay_ready_to_tick &= session->state[p].frame >= session->frame_counter;
        IMH(if                      (session->state[p].frame >= session->frame_counter + ULNET_DELAY_BUFFER_SIZE) { ImGui::Text("Input state on port %d is too new (ahead by %" PRId64 " frames)", p, session->state[p].frame - (session->frame_counter + ULNET_DELAY_BUFFER_SIZE)); })
            netplay_ready_to_tick &= session->state[p].frame <  session->frame_counter + ULNET_DELAY_BUFFER_SIZE; // This is needed for spectators only. By protocol it should always true for non-spectators unless we have a bug or someone is misbehaving
        }
    }

    bool ignore_frame_pacing_so_we_can_catch_up = false;
    if (ulnet_is_spectator(session, session->our_peer_id)) {
        int64_t authority_frame = -1;

        // The number of packets we check here is reasonable, since if we miss ULNET_DELAY_BUFFER_SIZE consecutive packets our connection is irrecoverable anyway
        for (int i = 0; i < ULNET_DELAY_BUFFER_SIZE; i++) {
            int64_t frame = -1;
            ulnet_state_packet_t *input_packet = (ulnet_state_packet_t *) session->state_packet_history[SAM2_AUTHORITY_INDEX][(session->frame_counter + i) % ULNET_STATE_PACKET_HISTORY_SIZE];
            rle8_decode(input_packet->coded_state, ULNET_PACKET_SIZE_BYTES_MAX, (uint8_t *) &frame, sizeof(frame));
            authority_frame = SAM2_MAX(authority_frame, frame);
        }

        int64_t max_frame_tolerance_a_peer_can_be_behind = 2 * session->delay_frames - 1;
        ignore_frame_pacing_so_we_can_catch_up = authority_frame > session->frame_counter + max_frame_tolerance_a_peer_can_be_behind;
    }

    if (!(session->frame_counter == ULNET_WAITING_FOR_SAVE_STATE_SENTINEL) && !ulnet_is_spectator(session, session->our_peer_id)) {
        int64_t frames_buffered = session->state[ulnet_our_port(session)].frame - session->frame_counter + 1;
        assert(frames_buffered <= ULNET_DELAY_BUFFER_SIZE);
        assert(frames_buffered >= 0);
    IMH(if                      (frames_buffered <  session->delay_frames) { ImGui::Text("We have not buffered enough frames still need %" PRId64, session->delay_frames - frames_buffered); })
        netplay_ready_to_tick &= frames_buffered >= session->delay_frames;
    }

    IMH(ImGui::End();)

    if (   netplay_ready_to_tick
        && (core_wants_tick_in_seconds(session->core_wants_tick_at_unix_usec) <= 0.0
        || ignore_frame_pacing_so_we_can_catch_up)) {
        status |= ULNET_POLL_SESSION_TICKED;
        // @todo I don't think this makes sense you should keep reasonable timing yourself if you can't the authority should just kick you
        //int64_t authority_is_on_frame = session->state[SAM2_AUTHORITY_INDEX].frame;

        int64_t target_frame_time_usec = 1000000 / frame_rate - 1000; // @todo There is a leftover millisecond bias here for some reason
        int64_t current_time_unix_usec = get_unix_time_microseconds();
        session->core_wants_tick_at_unix_usec = SAM2_MAX(session->core_wants_tick_at_unix_usec, current_time_unix_usec - target_frame_time_usec);
        session->core_wants_tick_at_unix_usec = SAM2_MIN(session->core_wants_tick_at_unix_usec, current_time_unix_usec + target_frame_time_usec);

        ulnet_core_option_t maybe_core_option_for_this_frame = session->state[SAM2_AUTHORITY_INDEX].core_option[session->frame_counter % ULNET_DELAY_BUFFER_SIZE];
        if (maybe_core_option_for_this_frame.key[0] != '\0') {
            if (strcmp(maybe_core_option_for_this_frame.key, "netplay_delay_frames") == 0) {
                session->delay_frames = atoi(maybe_core_option_for_this_frame.value);
            }

            for (int i = 0; i < SAM2_ARRAY_LENGTH(session->core_options); i++) {
                if (strcmp(session->core_options[i].key, maybe_core_option_for_this_frame.key) == 0) {
                    session->core_options[i] = maybe_core_option_for_this_frame;
                    session->flags |= ULNET_SESSION_FLAG_CORE_OPTIONS_DIRTY;
                    break;
                }
            }
        }

        session->flags &= ~ULNET_SESSION_FLAG_TICKED;
        int64_t save_state_frame = session->frame_counter;
        if (force_save_state_on_tick || session->peer_needs_sync_bitfield) {
            IMH(uint64_t start = rdtsc();)
            retro_serialize(save_state, save_state_size);
            IMH(g_save_cycle_count[g_frame_cyclic_offset] = rdtsc() - start;)
            status |= ULNET_POLL_SESSION_SAVED_STATE;

            if (session->flags & ULNET_SESSION_FLAG_TICKED) {
                SAM2_LOG_DEBUG("We ticked while saving state on frame %" PRId64, session->frame_counter);
                save_state_frame++; // @todo I think this is right I really need to write some kind of test though
            }
        }

        if (session->peer_needs_sync_bitfield) {
            for (uint64_t p = 0; p < SAM2_ARRAY_LENGTH(session->agent); p++) {
                if (session->peer_needs_sync_bitfield & (1ULL << p)) {
                    ulnet_send_save_state(session, session->agent[p], save_state, save_state_size, save_state_frame);
                    session->peer_needs_sync_bitfield &= ~(1ULL << p);
                }
            }
        }

        if (!(session->flags & ULNET_SESSION_FLAG_TICKED)) {
            retro_run();
        }

        session->core_wants_tick_at_unix_usec += 1000000 / frame_rate;

#if 0
        if (ulnet_is_authority(session)) {
            for (int p = 0; p < SAM2_PORT_MAX; p++) {
                sam2_room_t no_xor_delta = {0};
                sam2_room_t *suggested_room_xor_delta = &session->state[p].room_xor_delta[session->frame_counter % ULNET_DELAY_BUFFER_SIZE];
                if (memcmp(suggested_room_xor_delta, &no_xor_delta, sizeof(sam2_room_t)) != 0) {
                    sam2_room_join_message_t message = { SAM2_JOIN_HEADER };
                    message.room = session->room_we_are_in;
                    message.peer_id = session->room_we_are_in.peer_ids[p];
                    ulnet__xor_delta(&message.room, suggested_room_xor_delta, sizeof(sam2_room_t));
                    ulnet_process_message(session, &message);
                }
            }
        }
#endif

        sam2_room_t new_room_state = session->room_we_are_in;
        ulnet__xor_delta(&new_room_state, &session->state[SAM2_AUTHORITY_INDEX].room_xor_delta[session->frame_counter % ULNET_DELAY_BUFFER_SIZE], sizeof(sam2_room_t));

        if (memcmp(&new_room_state, &session->room_we_are_in, sizeof(sam2_room_t)) != 0) {
            SAM2_LOG_INFO("Something about the room we're in was changed by the authority");

            int64_t our_new_port = sam2_get_port_of_peer(&new_room_state, session->our_peer_id);
            if (   sam2_get_port_of_peer(&session->room_we_are_in, session->our_peer_id) == -1
                && our_new_port != -1) {
                // @todo This code can be reworked to remove the above if statement as is this conditional really doesn't make sense anyway, but it shouldn't really be a problem for now
                SAM2_LOG_INFO("We were let into the server by the authority");

                // @todo This assertion is only true if the peer left on their own and is behaving nicely
                assert(session->state[our_new_port].frame < session->frame_counter);
                session->state[our_new_port].frame = session->frame_counter;

                for (int p = 0; p < SAM2_ARRAY_LENGTH(new_room_state.peer_ids); p++) {
                    if (new_room_state.peer_ids[p] <= SAM2_PORT_SENTINELS_MAX) continue;
                    if (new_room_state.peer_ids[p] == session->our_peer_id) continue;
                    if (session->agent[p] == NULL) {
                        SAM2_LOG_INFO("Starting Interactive-Connectivity-Establishment for peer %016" PRIx64, new_room_state.peer_ids[p]);
                        ulnet_startup_ice_for_peer(session, new_room_state.peer_ids[p], NULL);
                    }
                }
            } else {
                for (int p = 0; p < SAM2_ARRAY_LENGTH(new_room_state.peer_ids); p++) {
                    // @todo Check something other than just joins and leaves
                    if (new_room_state.peer_ids[p] != session->room_we_are_in.peer_ids[p]) {
                        if (   session->room_we_are_in.peer_ids[p] > SAM2_PORT_SENTINELS_MAX
                            && new_room_state.peer_ids[p] <= SAM2_PORT_SENTINELS_MAX) {
                            if (session->room_we_are_in.peer_ids[p] == session->our_peer_id) {
                                SAM2_LOG_INFO("We were removed from port %d", p);
                                for (int peer_port = 0; peer_port < SAM2_PORT_MAX; peer_port++) {
                                    if (session->agent[peer_port]) {
                                        ulnet_disconnect_peer(session, peer_port);
                                    }
                                }
                            } else {
                                SAM2_LOG_INFO("Peer %" PRIx64 " has left the room", session->room_we_are_in.peer_ids[p]);
                                if (ulnet_is_authority(session)) {
                                    ulnet_move_peer(session, p, SAM2_PORT_MAX+1 + session->spectator_count++);
                                } else {
                                    ulnet_disconnect_peer(session, p);
                                }
                            }
                        } else if (new_room_state.peer_ids[p] > SAM2_PORT_SENTINELS_MAX) {
                            int peer_existing_port = ulnet_locate_peer(session, new_room_state.peer_ids[p]);
                            if (peer_existing_port != -1) {
                                SAM2_LOG_INFO("Spectator %016" PRIx64 " was promoted to peer", new_room_state.peer_ids[p]);
                                ulnet_move_peer(session, peer_existing_port, p); // This only moves spectators to real ports right now
                            }
                        }
                    }
                }
            }

            session->room_we_are_in = new_room_state;
            if (!(session->room_we_are_in.flags & SAM2_FLAG_ROOM_IS_NETWORK_HOSTED)) {
                SAM2_LOG_INFO("The room %016" PRIx64 ":'%s' was abandoned", session->room_we_are_in.peer_ids[SAM2_AUTHORITY_INDEX], session->room_we_are_in.name);
                for (int peer_port = 0; peer_port < SAM2_ARRAY_LENGTH(session->agent); peer_port++) {
                    if (session->agent[peer_port]) {
                        ulnet_disconnect_peer(session, peer_port);
                    }
                    session->room_we_are_in.peer_ids[peer_port] = SAM2_PORT_AVAILABLE;
                }
                ulnet_session_init_defaulted(session);
            }
        }

        if (session->room_we_are_in.flags & SAM2_FLAG_ROOM_IS_NETWORK_HOSTED) {
            session->desync_debug_packet.channel_and_flags = ULNET_CHANNEL_DESYNC_DEBUG;
            session->desync_debug_packet.frame          = save_state_frame;
            session->desync_debug_packet.save_state_hash [save_state_frame % ULNET_DELAY_BUFFER_SIZE] = ZSTD_XXH64(save_state, save_state_size, 0);
            //session->desync_debug_packet.input_state_hash[save_state_frame % ULNET_DELAY_BUFFER_SIZE] = ZSTD_XXH64(g_libretro_context.InputState, sizeof(g_libretro_context.InputState));

            for (int p = 0; p < SAM2_ARRAY_LENGTH(session->agent); p++) {
                if (!session->agent[p]) continue;
                juice_state_t juice_state = juice_get_state(session->agent[p]);
                if (   (juice_state == JUICE_STATE_CONNECTED || juice_state == JUICE_STATE_COMPLETED)
                    && !ulnet_is_spectator(session, session->our_peer_id)) {
                    juice_send(session->agent[p], (char *) &session->desync_debug_packet, sizeof(session->desync_debug_packet));
                }
            }
        }

        // Ideally I'd place this right after ticking the core, but we need to update the room state first
        session->frame_counter++;
    }

    return status;
}

ULNET_LINKAGE void ulnet_move_peer(ulnet_session_t *session, int peer_existing_port, int peer_new_port) {
    assert(peer_new_port == -1 || peer_existing_port != peer_new_port);
    assert(peer_new_port == -1 || session->agent[peer_new_port] == NULL);
    assert(peer_new_port == -1 || session->room_we_are_in.peer_ids[peer_new_port] <= SAM2_PORT_SENTINELS_MAX);
    assert(session->agent[peer_existing_port] != NULL);

    juice_agent_t *agent = session->agent[peer_existing_port];
    int64_t peer_id = session->room_we_are_in.peer_ids[peer_existing_port];

    session->agent[peer_existing_port] = NULL;
    session->room_we_are_in.peer_ids[peer_existing_port] = 0;

    if (peer_new_port == -1) {
        juice_destroy(agent);
    } else {
        session->agent[peer_new_port] = agent;
        session->room_we_are_in.peer_ids[peer_new_port] = peer_id;
    }

    if (peer_existing_port > SAM2_AUTHORITY_INDEX) {
        // Remove with replacement (Spectators are stored contiguously with no gaps)
        // @todo Maybe don't do this if it makes the implementation easier
        assert(session->spectator_count > 0);
        assert(peer_new_port < SAM2_PORT_MAX);
        assert(peer_new_port - (SAM2_PORT_MAX + 1) < session->spectator_count);

        session->spectator_count--;
        session->agent[peer_existing_port] = session->agent[(SAM2_PORT_MAX+1) + session->spectator_count];
        session->agent[(SAM2_PORT_MAX+1) + session->spectator_count] = NULL;
        session->room_we_are_in.peer_ids[peer_existing_port] = session->spectator_peer_ids[session->spectator_count];
    }
}

ULNET_LINKAGE void ulnet_disconnect_peer(ulnet_session_t *session, int peer_port) {
    if (peer_port > SAM2_AUTHORITY_INDEX) {
        SAM2_LOG_INFO("Disconnecting spectator %016" PRIx64, session->room_we_are_in.peer_ids[peer_port]);
    } else {
        SAM2_LOG_INFO("Disconnecting Peer %016" PRIx64, session->room_we_are_in.peer_ids[peer_port]);
    }

    ulnet_move_peer(session, peer_port, -1);
}

static inline void ulnet__reset_save_state_bookkeeping(ulnet_session_t *session) {
    session->remote_packet_groups = FEC_PACKET_GROUPS_MAX;
    session->remote_savestate_transfer_offset = 0;
    memset(session->fec_index_counter, 0, sizeof(session->fec_index_counter));
}

ULNET_LINKAGE void ulnet_session_init_defaulted(ulnet_session_t *session) {
    assert(session->spectator_count == 0);
    for (int i = 0; i < SAM2_PORT_MAX+1; i++) {
        assert(session->agent[i] == NULL);
    }

    memset(&session->state, 0, sizeof(session->state));
    memset(&session->state_packet_history, 0, sizeof(session->state_packet_history));

    session->frame_counter = 0;
    session->room_we_are_in.peer_ids[SAM2_AUTHORITY_INDEX] = session->our_peer_id;

    ulnet__reset_save_state_bookkeeping(session);
}

// MARK: libjuice callbacks
static void ulnet__on_state_changed(juice_agent_t *agent, juice_state_t state, void *user_ptr) {
    ulnet_session_t *session = (ulnet_session_t *) user_ptr;

    int p;
    SAM2_LOCATE(session->agent, agent, p);

    if (   state == JUICE_STATE_CONNECTED
        && session->our_peer_id == session->room_we_are_in.peer_ids[SAM2_AUTHORITY_INDEX]) {
        SAM2_LOG_INFO("Setting peer needs sync bit for peer %016" PRIx64, session->our_peer_id);
        session->peer_needs_sync_bitfield |= (1ULL << p);
    } else if (state == JUICE_STATE_FAILED) {
        if (p >= SAM2_PORT_MAX+1) {
            SAM2_LOG_INFO("Spectator %016" PRIx64 " left" , session->room_we_are_in.peer_ids[p]);
            ulnet_disconnect_peer(session, p);
        } else {

        }

    }
}

// On local candidate gathered
static void ulnet__on_candidate(juice_agent_t *agent, const char *sdp, void *user_ptr) {
    ulnet_session_t *session = (ulnet_session_t *) user_ptr;

    int p;
    SAM2_LOCATE(session->agent, agent, p);
    if (p == -1) {
        SAM2_LOG_ERROR("No agent found");
        return;
    }

    sam2_signal_message_t response = { SAM2_SIGN_HEADER };

    response.peer_id = session->room_we_are_in.peer_ids[p];
    if (strlen(sdp) < sizeof(response.ice_sdp)) {
        strcpy(response.ice_sdp, sdp);
        session->sam2_send_callback(session->user_ptr, (char *) &response);
    } else {
        SAM2_LOG_ERROR("Candidate too large");
        return;
    }
}

// On local candidates gathering done
static void ulnet__on_gathering_done(juice_agent_t *agent, void *user_ptr) {
    ulnet_session_t *session = (ulnet_session_t *) user_ptr;

    int p;
    SAM2_LOCATE(session->agent, agent, p);
    if (p == -1) {
        SAM2_LOG_ERROR("No agent found");
        return;
    }

    sam2_signal_message_t response = { SAM2_SIGN_HEADER };

    response.peer_id = session->room_we_are_in.peer_ids[p];
    session->sam2_send_callback(session->user_ptr, (char *) &response);
}

static void ulnet_receive_packet_callback(juice_agent_t *agent, const char *data, size_t size, void *user_ptr) {
    ulnet_session_t *session = (ulnet_session_t *) user_ptr;

    int p;
    SAM2_LOCATE(session->agent, agent, p);

    if (p == -1) {
        SAM2_LOG_ERROR("No agent associated for packet on channel 0x%" PRIx8 "", data[0] & ULNET_CHANNEL_MASK);
        return;
    }

    if (size == 0) {
        SAM2_LOG_WARN("Received a UDP packet with no payload");
        return;
    }

    if (p >= SAM2_PORT_MAX+1) {
        SAM2_LOG_WARN("A spectator sent us a UDP packet for unsupported channel %" PRIx8 " for some reason", data[0] & ULNET_CHANNEL_MASK);
        return;
    }

    uint8_t channel_and_flags = data[0];
    switch (channel_and_flags & ULNET_CHANNEL_MASK) {
    case ULNET_CHANNEL_EXTRA: {
        assert(!"This is an error currently\n");
        break;
    }
    case ULNET_CHANNEL_INPUT: {
        assert(size <= ULNET_PACKET_SIZE_BYTES_MAX);

        ulnet_state_packet_t *input_packet = (ulnet_state_packet_t *) data;
        int8_t original_sender_port = data[0] & ULNET_FLAGS_MASK;

        if (   p != original_sender_port
            && p != SAM2_AUTHORITY_INDEX) {
            SAM2_LOG_WARN("Non-authority gave us someones input eventually this should be verified with a signature");
        }

        if (original_sender_port >= SAM2_PORT_MAX+1) {
            SAM2_LOG_WARN("Received input packet for port %d which is out of range", original_sender_port);
            break;
        }

        if (rle8_decode_size(input_packet->coded_state, size - 1) != sizeof(ulnet_state_t)) {
            SAM2_LOG_WARN("Received input packet with an invalid decode size");
            break;
        }

        int64_t frame;
        rle8_decode(input_packet->coded_state, size - 1, (uint8_t *) &frame, sizeof(frame));

        SAM2_LOG_DEBUG("Recv input packet for frame %" PRId64 " from peer_ids[%d]=%" PRIx64 "",
            frame, original_sender_port, session->room_we_are_in.peer_ids[original_sender_port]);

        if (frame < session->state[original_sender_port].frame) {
            // UDP packets can arrive out of order this is normal
            SAM2_LOG_DEBUG("Received outdated input packet for frame %" PRId64 ". We are already on frame %" PRId64 ". Dropping it",
                frame, session->state[original_sender_port].frame);
        } else {
            rle8_decode(
                input_packet->coded_state, size - 1,
                (uint8_t *) &session->state[original_sender_port], sizeof(ulnet_state_t)
            );

            // Store the input packet in the history buffer. Arbitrary zero runs decode to no bytes conveniently so we don't need to store the packet size
            int i = 0;
            for (; i < size; i++) {
                session->state_packet_history[original_sender_port][frame % ULNET_STATE_PACKET_HISTORY_SIZE][i] = data[i];
            }

            for (; i < SAM2_ARRAY_LENGTH(session->state_packet_history[0][0]); i++) {
                session->state_packet_history[original_sender_port][frame % ULNET_STATE_PACKET_HISTORY_SIZE][i] = 0;
            }

            // Broadcast the input packet to spectators
            if (ulnet_is_authority(session)) {
                for (i = 0; i < ULNET_SPECTATOR_MAX; i++) {
                    juice_agent_t *spectator_agent = session->agent[SAM2_PORT_MAX+1 + i];
                    if (spectator_agent) {
                        if (   juice_get_state(spectator_agent) == JUICE_STATE_CONNECTED
                            || juice_get_state(spectator_agent) == JUICE_STATE_COMPLETED) {
                            int status = juice_send(spectator_agent, data, size);
                            assert(status == 0);
                        }
                    }
                }
            }
        }

        break;
    }
    case ULNET_CHANNEL_DESYNC_DEBUG: {
        // @todo This channel doesn't receive messages reliably, but I think it should be changed to in the same manner as the input channel
        assert(size == sizeof(desync_debug_packet_t));

        desync_debug_packet_t their_desync_debug_packet;
        memcpy(&their_desync_debug_packet, data, sizeof(desync_debug_packet_t)); // Strict-aliasing

        desync_debug_packet_t our_desync_debug_packet = session->desync_debug_packet;

        int64_t latest_common_frame = SAM2_MIN(our_desync_debug_packet.frame, their_desync_debug_packet.frame);
        int64_t frame_difference = SAM2_ABS(our_desync_debug_packet.frame - their_desync_debug_packet.frame);
        int64_t total_frames_to_compare = ULNET_DELAY_BUFFER_SIZE - frame_difference;
        for (int f = total_frames_to_compare-1; f >= 0 ; f--) {
            int64_t frame_to_compare = latest_common_frame - f;
            int64_t frame_index = frame_to_compare % ULNET_DELAY_BUFFER_SIZE;

            if (our_desync_debug_packet.input_state_hash[frame_index] != their_desync_debug_packet.input_state_hash[frame_index]) {
                SAM2_LOG_ERROR("Input state hash mismatch for frame %" PRId64 " Our hash: %" PRIx64 " Their hash: %" PRIx64 "", 
                    frame_to_compare, our_desync_debug_packet.input_state_hash[frame_index], their_desync_debug_packet.input_state_hash[frame_index]);
            } else if (   our_desync_debug_packet.save_state_hash[frame_index]
                       && their_desync_debug_packet.save_state_hash[frame_index]) {

                if (our_desync_debug_packet.save_state_hash[frame_index] != their_desync_debug_packet.save_state_hash[frame_index]) {
                    if (!session->peer_desynced_frame[p]) {
                        session->peer_desynced_frame[p] = frame_to_compare;
                    }

                    SAM2_LOG_ERROR("Save state hash mismatch for frame %" PRId64 " Our hash: %016" PRIx64 " Their hash: %016" PRIx64 "",
                        frame_to_compare, our_desync_debug_packet.save_state_hash[frame_index], their_desync_debug_packet.save_state_hash[frame_index]);
                } else if (session->peer_desynced_frame[p]) {
                    session->peer_desynced_frame[p] = 0;
                    SAM2_LOG_INFO("Peer resynced frame on frame %" PRId64 "", frame_to_compare);
                }
            }
        }

        break;
    }
    case ULNET_CHANNEL_SAVESTATE_TRANSFER: {
        if (session->remote_packet_groups == 0) {
            // This is kind of a hack. Since every field in ulnet_session can just be zero-inited
            // besides this one. I just use this check here to set it to it's correct initial value
            session->remote_packet_groups = FEC_PACKET_GROUPS_MAX;
        }

        if (session->agent[SAM2_AUTHORITY_INDEX] != agent) {
            printf("Received savestate transfer packet from non-authority agent\n");
            break;
        }

        if (size < sizeof(ulnet_save_state_packet_header_t)) {
            SAM2_LOG_WARN("Recv savestate transfer packet with size smaller than header");
            break;
        }

        if (size > ULNET_PACKET_SIZE_BYTES_MAX) {
            SAM2_LOG_WARN("Recv savestate transfer packet potentially larger than MTU");
        }

        ulnet_save_state_packet_header_t savestate_transfer_header;
        memcpy(&savestate_transfer_header, data, sizeof(ulnet_save_state_packet_header_t)); // Strict-aliasing

        uint8_t sequence_hi = 0;
        int k = 239;
        if (channel_and_flags & ULNET_SAVESTATE_TRANSFER_FLAG_K_IS_239) {
            if (channel_and_flags & ULNET_SAVESTATE_TRANSFER_FLAG_SEQUENCE_HI_IS_0) {
                session->remote_packet_groups = savestate_transfer_header.packet_groups;
            } else {
                sequence_hi = savestate_transfer_header.sequence_hi;
            }
        } else {
            k = savestate_transfer_header.reed_solomon_k;
            session->remote_packet_groups = 1; // k != 239 => 1 packet group
        }

        if (session->fec_index_counter[sequence_hi] == k) {
            // We already have received enough Reed-Solomon blocks to decode the payload; we can ignore this packet
            break;
        }

        if (sequence_hi >= FEC_PACKET_GROUPS_MAX) {
            SAM2_LOG_WARN("Received savestate transfer packet with sequence_hi >= FEC_PACKET_GROUPS_MAX");
            break;
        }

        uint8_t sequence_lo = savestate_transfer_header.sequence_lo;

        SAM2_LOG_DEBUG("Received savestate packet sequence_hi: %hhu sequence_lo: %hhu", sequence_hi, sequence_lo);

        uint8_t *copied_packet_ptr = (uint8_t *) memcpy(&session->remote_savestate_transfer_packets[session->remote_savestate_transfer_offset], data, size);
        session->remote_savestate_transfer_offset += size;

        session->fec_packet[sequence_hi][session->fec_index_counter[sequence_hi]] = copied_packet_ptr + sizeof(ulnet_save_state_packet_header_t);
        session->fec_index [sequence_hi][session->fec_index_counter[sequence_hi]++] = sequence_lo;

        if (session->fec_index_counter[sequence_hi] == k) {
            SAM2_LOG_DEBUG("Received all the savestate data for packet group: %hhu", sequence_hi);

            int redudant_blocks_sent = k * FEC_REDUNDANT_BLOCKS / (GF_SIZE - FEC_REDUNDANT_BLOCKS);
            void *rs_code = fec_new(k, k + redudant_blocks_sent);
            int rs_block_size = (int) (size - sizeof(ulnet_save_state_packet_header_t));
            int status = fec_decode(rs_code, session->fec_packet[sequence_hi], session->fec_index[sequence_hi], rs_block_size);
            assert(status == 0);
            fec_free(rs_code);

            bool all_data_decoded = true;
            for (int i = 0; i < session->remote_packet_groups; i++) {
                all_data_decoded &= session->fec_index_counter[i] >= k;
            }

            if (all_data_decoded) {
                size_t ret = 0;
                uint64_t their_savestate_transfer_payload_xxhash = 0;
                uint64_t   our_savestate_transfer_payload_xxhash = 0;
                unsigned char *save_state_data = NULL;
                savestate_transfer_payload_t *savestate_transfer_payload = (savestate_transfer_payload_t *) malloc(sizeof(savestate_transfer_payload_t) /* Fixed size header */ + COMPRESSED_DATA_WITH_REDUNDANCY_BOUND_BYTES);

                int64_t remote_payload_size = 0;
                // @todo The last packet contains some number of garbage bytes probably add the size thing back?
                for (int i = 0; i < k; i++) {
                    for (int j = 0; j < session->remote_packet_groups; j++) {
                        memcpy(((uint8_t *) savestate_transfer_payload) + remote_payload_size, session->fec_packet[j][i], rs_block_size);
                        remote_payload_size += rs_block_size;
                    }
                }

                SAM2_LOG_INFO("Received savestate transfer payload for frame %" PRId64 "", savestate_transfer_payload->frame_counter);

                if (   savestate_transfer_payload->total_size_bytes > k * (int) rs_block_size * session->remote_packet_groups
                    || savestate_transfer_payload->total_size_bytes < 0) {
                    SAM2_LOG_ERROR("Savestate transfer payload total size would out-of-bounds when computing hash: %" PRId64 "", savestate_transfer_payload->total_size_bytes);
                    goto cleanup;
                }

                their_savestate_transfer_payload_xxhash = savestate_transfer_payload->xxhash;
                savestate_transfer_payload->xxhash = 0;
                our_savestate_transfer_payload_xxhash = ZSTD_XXH64(savestate_transfer_payload, savestate_transfer_payload->total_size_bytes, 0);

                if (their_savestate_transfer_payload_xxhash != our_savestate_transfer_payload_xxhash) {
                    SAM2_LOG_ERROR("Savestate transfer payload hash mismatch: %" PRIx64 " != %" PRIx64 "", their_savestate_transfer_payload_xxhash, our_savestate_transfer_payload_xxhash);
                    goto cleanup;
                }

                ret = ZSTD_decompress(
                    session->core_options, sizeof(session->core_options),
                    savestate_transfer_payload->compressed_data + savestate_transfer_payload->compressed_savestate_size,
                    savestate_transfer_payload->compressed_options_size
                );

                if (ZSTD_isError(ret)) {
                    SAM2_LOG_ERROR("Error decompressing core options: %s", ZSTD_getErrorName(ret));
                } else {
                    session->flags |= ULNET_SESSION_FLAG_CORE_OPTIONS_DIRTY;
                    //session.retro_run(); // Apply options before loading savestate; Lets hope this isn't necessary

                    save_state_data = (unsigned char *) malloc(savestate_transfer_payload->decompressed_savestate_size);

                    int64_t save_state_size = ZSTD_decompress(
                        save_state_data,
                        savestate_transfer_payload->decompressed_savestate_size,
                        savestate_transfer_payload->compressed_data,
                        savestate_transfer_payload->compressed_savestate_size
                    );

                    if (ZSTD_isError(save_state_size)) {
                        SAM2_LOG_ERROR("Error decompressing savestate: %s", ZSTD_getErrorName(save_state_size));
                    } else {
                        if (!session->retro_unserialize(save_state_data, save_state_size)) {
                            SAM2_LOG_ERROR("Failed to load savestate");
                        } else {
                            SAM2_LOG_DEBUG("Save state loaded");
                            session->frame_counter = savestate_transfer_payload->frame_counter;
                            session->room_we_are_in = savestate_transfer_payload->room;
                        }
                    }
                }

cleanup:
                if (save_state_data != NULL) {
                    free(save_state_data);
                }

                free(savestate_transfer_payload);

                ulnet__reset_save_state_bookkeeping(session);
            }
        }
        break;
    }
    default:
        SAM2_LOG_WARN("Unknown channel: %d", channel_and_flags);
    }
}

ULNET_LINKAGE int ulnet_startup_ice_for_peer(ulnet_session_t *session, uint64_t peer_id, const char *remote_description) {
    juice_config_t config;
    memset(&config, 0, sizeof(config));

    // STUN server example*
    config.concurrency_mode = JUICE_CONCURRENCY_MODE_USER;
    config.stun_server_host = "stun2.l.google.com"; // @todo Put a bad url here to test how to handle that
    config.stun_server_port = 19302;
    //config.bind_address = "127.0.0.1";

    config.cb_state_changed = ulnet__on_state_changed;
    config.cb_candidate = ulnet__on_candidate;
    config.cb_gathering_done = ulnet__on_gathering_done;
    config.cb_recv = ulnet_receive_packet_callback;

    config.user_ptr = (void *) session;

    int p = sam2_get_port_of_peer(&session->room_we_are_in, peer_id);
    if (p == -1) {
        assert(session->spectator_count < SAM2_ARRAY_LENGTH(session->spectator_peer_ids));
        session->spectator_peer_ids[p = session->spectator_count++] = peer_id;
        p += SAM2_PORT_MAX + 1;
    }

    assert(session->agent[p] == NULL);
    session->agent[p] = juice_create(&config);

    if (remote_description) {
        // Right now I think there could be some kind of bug or race condition in my code or libjuice when there
        // is an ICE role conflict. A role conflict is benign, but when a spectator connects the authority will never fully
        // establish the connection even though the spectator manages to. If I avoid the role conflict by setting
        // the remote description here then my connection establishes fine, but I should look into this eventually @todo
        juice_set_remote_description(session->agent[p], remote_description);
    }

    sam2_signal_message_t signal_message = { SAM2_SIGN_HEADER };
    signal_message.peer_id = peer_id;
    juice_get_local_description(session->agent[p], signal_message.ice_sdp, sizeof(signal_message.ice_sdp));
    session->sam2_send_callback(session->user_ptr, (char *) &signal_message);

    // This call starts an asynchronous task that requires periodic polling via juice_user_poll to complete
    // it will call the ulnet__on_gathering_done callback once it's finished
    juice_gather_candidates(session->agent[p]);

    return p;
}

int ulnet_process_message(ulnet_session_t *session, void *response) {

    if (sam2_get_metadata((char *) response) == NULL) {
        return -1;
    }

    if (memcmp(response, sam2_make_header, SAM2_HEADER_TAG_SIZE) == 0) {
        sam2_room_make_message_t *room_make = (sam2_room_make_message_t *) response;
        assert(session->our_peer_id == room_make->room.peer_ids[SAM2_AUTHORITY_INDEX]);
        assert(!(session->room_we_are_in.flags & SAM2_FLAG_ROOM_IS_NETWORK_HOSTED));
        session->room_we_are_in = room_make->room;
    } else if (memcmp(response, sam2_conn_header, SAM2_HEADER_TAG_SIZE) == 0) {
        sam2_connect_message_t *connect_message = (sam2_connect_message_t *) response;
        SAM2_LOG_INFO("We were assigned the peer id %" PRIx64, connect_message->peer_id);

        session->our_peer_id = connect_message->peer_id;
        session->room_we_are_in.peer_ids[SAM2_AUTHORITY_INDEX] = session->our_peer_id;
    } else if (memcmp(response, sam2_join_header, SAM2_HEADER_TAG_SIZE) == 0) {
        if (!ulnet_is_authority(session)) {
            SAM2_LOG_FATAL("We shouldn't get here anymore"); // @todo Make error instead
        }
        sam2_room_join_message_t *room_join = (sam2_room_join_message_t *) response;

        // This looks weird but really we're just figuring out what the current state of the room
        // looks like so we can generate deltas against it
        sam2_room_t future_room_we_are_in = session->room_we_are_in;
        for (int64_t frame = session->frame_counter+1LL; frame < session->state[SAM2_AUTHORITY_INDEX].frame; frame++) {
            ulnet__xor_delta(
                &future_room_we_are_in,
                &session->state[SAM2_AUTHORITY_INDEX].room_xor_delta[frame % ULNET_DELAY_BUFFER_SIZE],
                sizeof(session->room_we_are_in)
            );
        }
        ulnet__xor_delta(&future_room_we_are_in, &session->next_room_xor_delta, sizeof(session->room_we_are_in));

        SAM2_LOG_INFO("Peer %" PRIx64 " has asked to change something about the room in some way e.g. leaving, joining, etc.", room_join->peer_id);
        assert(sam2_same_room(&future_room_we_are_in, &room_join->room));

        int current_port = sam2_get_port_of_peer(&future_room_we_are_in, room_join->peer_id);
        int desired_port = sam2_get_port_of_peer(&room_join->room, room_join->peer_id);

        if (desired_port == -1) {
            if (current_port != -1) {
                SAM2_LOG_INFO("Peer %" PRIx64 " left", room_join->peer_id);

                session->next_room_xor_delta.peer_ids[current_port] = future_room_we_are_in.peer_ids[current_port] ^ SAM2_PORT_AVAILABLE;
            } else {
                SAM2_LOG_WARN("Peer %" PRIx64 " did something that doesn't look like joining or leaving", room_join->peer_id);

                sam2_error_message_t error = {
                    SAM2_FAIL_HEADER,
                    SAM2_RESPONSE_AUTHORITY_ERROR,
                    "Client made unsupported join request",
                    room_join->peer_id
                };

                session->sam2_send_callback(session->user_ptr, (char *) &error);
            }
        } else {
            if (current_port != desired_port) {
                if (future_room_we_are_in.peer_ids[desired_port] != SAM2_PORT_AVAILABLE) {
                    SAM2_LOG_INFO("Peer %" PRIx64 " tried to join on unavailable port", room_join->room.peer_ids[current_port]);
                    sam2_error_message_t error = {
                        SAM2_FAIL_HEADER,
                        SAM2_RESPONSE_AUTHORITY_ERROR,
                        "Peer tried to join on unavailable port",
                        room_join->peer_id
                    };

                    session->sam2_send_callback(session->user_ptr, (char *) &error);
                } else {
                    session->next_room_xor_delta.peer_ids[desired_port] = future_room_we_are_in.peer_ids[desired_port] ^ room_join->peer_id;

                    if (current_port != -1) {
                        future_room_we_are_in.peer_ids[current_port] = SAM2_PORT_AVAILABLE;
                    }
                }
            }
        }

        // @todo We should mask for values peers are allowed to change
        if (room_join->peer_id == session->our_peer_id) {
            session->next_room_xor_delta.flags = future_room_we_are_in.flags ^ room_join->room.flags;
        }

        sam2_room_t no_xor_delta = {0};
        if (memcmp(&session->next_room_xor_delta, &no_xor_delta, sizeof(sam2_room_t)) == 0) {
            SAM2_LOG_WARN("Peer %" PRIx64 " didn't change anything after making join request", room_join->peer_id);
        } else {
            ulnet__xor_delta(&future_room_we_are_in, &session->next_room_xor_delta, sizeof(session->room_we_are_in));
            sam2_room_make_message_t make_message = {
                SAM2_MAKE_HEADER,
                future_room_we_are_in
            };

            session->sam2_send_callback(session->user_ptr, (char *) &make_message);
        }
    } else if (   memcmp(response, sam2_sign_header, SAM2_HEADER_TAG_SIZE) == 0
               || memcmp(response, sam2_sigx_header, SAM2_HEADER_TAG_SIZE) == 0) {
        sam2_signal_message_t *room_signal = (sam2_signal_message_t *) response;
        SAM2_LOG_INFO("Received signal from peer %" PRIx64 "", room_signal->peer_id);

        if (!(session->room_we_are_in.flags & SAM2_FLAG_ROOM_IS_NETWORK_HOSTED)) {
            SAM2_LOG_WARN("Ignoring signal from %016" PRIx64 ". We aren't in a netplay session presently", room_signal->peer_id);
            return 0;
        }

        int p = ulnet_locate_peer(session, room_signal->peer_id);

        if (p == -1) {
            SAM2_LOG_INFO("Received signal from unknown peer");

            if (session->our_peer_id == session->room_we_are_in.peer_ids[SAM2_AUTHORITY_INDEX]) {
                if (session->spectator_count == ULNET_SPECTATOR_MAX) {
                    SAM2_LOG_WARN("We can't let them in as a spectator there are too many spectators");

                    static sam2_error_message_t error = { 
                        SAM2_FAIL_HEADER,
                        SAM2_RESPONSE_AUTHORITY_ERROR,
                        "Authority has reached the maximum number of spectators"
                    };

                    session->sam2_send_callback(session->user_ptr, (char *) &error);
                } else {
                    SAM2_LOG_INFO("We are letting them in as a spectator");
                }
            } else {
                SAM2_LOG_WARN("Received unknown signal when we weren't the authority");

                static sam2_error_message_t error = { 
                    SAM2_FAIL_HEADER,
                    SAM2_RESPONSE_AUTHORITY_ERROR,
                    "Received unknown signal when we weren't the authority"
                };

                error.peer_id = room_signal->peer_id;

                session->sam2_send_callback(session->user_ptr, (char *) &error);
            }
        }

        if (session->agent[p] == NULL) {
            // Have to get move the peer once we know which port it belongs to
            p = ulnet_startup_ice_for_peer(
                session,
                room_signal->peer_id,
                /* remote_desciption = */ room_signal->ice_sdp
            );
        }

        if (p != -1) {
            if (room_signal->header[3] == 'X') {
                if (p > SAM2_AUTHORITY_INDEX) {
                    ulnet_disconnect_peer(session, p);
                } else {
                    SAM2_LOG_WARN("Protocol violation: room.peer_ids[%d]=%016" PRIx64 " signaled disconnect before exiting room", p, room_signal->peer_id);
                    sam2_error_message_t error = {
                        SAM2_FAIL_HEADER,
                        SAM2_RESPONSE_AUTHORITY_ERROR,
                        "Protocol violation: Signaled disconnect before detatching port",
                        room_signal->peer_id
                    };

                    session->sam2_send_callback(session->user_ptr, (char *) &error);
                    // @todo Resync broadcast
                }
            } else if (room_signal->ice_sdp[0] == '\0') {
                SAM2_LOG_INFO("Received remote gathering done from peer %" PRIx64 "", room_signal->peer_id);
                juice_set_remote_gathering_done(session->agent[p]);
            } else if (strncmp(room_signal->ice_sdp, "a=ice", strlen("a=ice")) == 0) {
                juice_set_remote_description(session->agent[p], room_signal->ice_sdp);
            } else if (strncmp(room_signal->ice_sdp, "a=candidate", strlen("a=candidate")) == 0) {
                juice_add_remote_candidate(session->agent[p], room_signal->ice_sdp);
            } else {
                SAM2_LOG_ERROR("Unable to parse signal message '%s'", room_signal->ice_sdp);
            }
        }
    }

    return 0;
}

// Pass in save state since often retro_serialize can tick the core
ULNET_LINKAGE void ulnet_send_save_state(ulnet_session_t *session, juice_agent_t *agent, void *save_state, size_t save_state_size, int64_t save_state_frame) {
    assert(save_state);

    int packet_payload_size_bytes = ULNET_PACKET_SIZE_BYTES_MAX - sizeof(ulnet_save_state_packet_header_t);
    int n, k, packet_groups;

    int64_t save_state_transfer_payload_compressed_bound_size_bytes = ZSTD_COMPRESSBOUND(save_state_size) + ZSTD_COMPRESSBOUND(sizeof(session->core_options));
    ulnet__logical_partition(sizeof(savestate_transfer_payload_t) /* Header */ + save_state_transfer_payload_compressed_bound_size_bytes,
                      FEC_REDUNDANT_BLOCKS, &n, &k, &packet_payload_size_bytes, &packet_groups);

    size_t savestate_transfer_payload_plus_parity_bound_bytes = packet_groups * n * packet_payload_size_bytes;

    // This points to the savestate transfer payload, but also the remaining bytes at the end hold our parity blocks
    // Having this data in a single contiguous buffer makes indexing easier
    savestate_transfer_payload_t *savestate_transfer_payload = (savestate_transfer_payload_t *) malloc(savestate_transfer_payload_plus_parity_bound_bytes);

    savestate_transfer_payload->decompressed_savestate_size = save_state_size;
    savestate_transfer_payload->compressed_savestate_size = ZSTD_compress(
        savestate_transfer_payload->compressed_data,
        save_state_transfer_payload_compressed_bound_size_bytes,
        save_state, save_state_size, session->zstd_compress_level
    );

    if (ZSTD_isError(savestate_transfer_payload->compressed_savestate_size)) {
        SAM2_LOG_ERROR("ZSTD_compress failed: %s", ZSTD_getErrorName(savestate_transfer_payload->compressed_savestate_size));
        assert(0);
    }

    savestate_transfer_payload->compressed_options_size = ZSTD_compress(
        savestate_transfer_payload->compressed_data + savestate_transfer_payload->compressed_savestate_size,
        save_state_transfer_payload_compressed_bound_size_bytes - savestate_transfer_payload->compressed_savestate_size,
        session->core_options, sizeof(session->core_options), session->zstd_compress_level
    );

    if (ZSTD_isError(savestate_transfer_payload->compressed_options_size)) {
        SAM2_LOG_ERROR("ZSTD_compress failed: %s", ZSTD_getErrorName(savestate_transfer_payload->compressed_options_size));
        assert(0);
    }

    ulnet__logical_partition(
        sizeof(savestate_transfer_payload_t) /* Header */ + savestate_transfer_payload->compressed_savestate_size + savestate_transfer_payload->compressed_options_size,
        FEC_REDUNDANT_BLOCKS, &n, &k, &packet_payload_size_bytes, &packet_groups
    );
    assert(savestate_transfer_payload_plus_parity_bound_bytes >= packet_groups * n * packet_payload_size_bytes); // If this fails my logic calculating the bounds was just wrong

    savestate_transfer_payload->frame_counter = save_state_frame;
    savestate_transfer_payload->room = session->room_we_are_in;
    savestate_transfer_payload->total_size_bytes = sizeof(savestate_transfer_payload_t) + savestate_transfer_payload->compressed_savestate_size + savestate_transfer_payload->compressed_options_size;

    savestate_transfer_payload->xxhash = 0;
    savestate_transfer_payload->xxhash = ZSTD_XXH64(savestate_transfer_payload, savestate_transfer_payload->total_size_bytes, 0);
    // Create parity blocks for Reed-Solomon. n - k in total for each packet group
    // We have "packet grouping" because pretty much every implementation of Reed-Solomon doesn't support more than 255 blocks
    // and unfragmented UDP packets over ethernet are limited to ULNET_PACKET_SIZE_BYTES_MAX
    // This makes the code more complicated and the error correcting properties slightly worse but it's a practical tradeoff
    void *rs_code = fec_new(k, n);
    for (int j = 0; j < packet_groups; j++) {
        void *data[255];

        for (int i = 0; i < n; i++) {
            data[i] = (unsigned char *) savestate_transfer_payload + ulnet__logical_partition_offset_bytes(j, i, packet_payload_size_bytes, packet_groups);
        }

        for (int i = k; i < n; i++) {
            fec_encode(rs_code, (void **)data, data[i], i, packet_payload_size_bytes);
        }
    }
    fec_free(rs_code);

    // Send original data blocks and parity blocks
    // @todo I wrote this in such a way that you can do a zero-copy when creating the packets to send
    for (int i = 0; i < n; i++) {
        for (int j = 0; j < packet_groups; j++) {
            ulnet_save_state_packet_fragment2_t packet;
            packet.channel_and_flags = ULNET_CHANNEL_SAVESTATE_TRANSFER;
            if (k == 239) {
                packet.channel_and_flags |= ULNET_SAVESTATE_TRANSFER_FLAG_K_IS_239;
                if (j == 0) {
                    packet.channel_and_flags |= ULNET_SAVESTATE_TRANSFER_FLAG_SEQUENCE_HI_IS_0;
                    packet.packet_groups = packet_groups;
                } else {
                    packet.sequence_hi = j;
                }
            } else {
                packet.reed_solomon_k = k;
            }

            packet.sequence_lo = i;

            memcpy(packet.payload, (unsigned char *) savestate_transfer_payload + ulnet__logical_partition_offset_bytes(j, i, packet_payload_size_bytes, packet_groups), packet_payload_size_bytes);

            int status = juice_send(agent, (char *) &packet, sizeof(ulnet_save_state_packet_header_t) + packet_payload_size_bytes);
            assert(status == 0);
        }
    }

    free(savestate_transfer_payload);
}
#endif
