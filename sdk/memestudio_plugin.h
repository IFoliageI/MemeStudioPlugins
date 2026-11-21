#ifndef MEMESTUDIO_PLUGIN_H
#define MEMESTUDIO_PLUGIN_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MEMESTUDIO_PLUGIN_ABI_V2 2u
#define MEMESTUDIO_PLUGIN_ENTRY_V2 "MemeStudioGetPluginV2"

#ifdef _WIN32
#define MEMESTUDIO_CALL __cdecl
#define MEMESTUDIO_EXPORT __declspec(dllexport)
#else
#define MEMESTUDIO_CALL
#define MEMESTUDIO_EXPORT
#endif

typedef struct MemeStudioBytesV2 {
    uint8_t* data;
    size_t size;
} MemeStudioBytesV2;

typedef struct MemeStudioImageBufferV2 {
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t delay_ms;
    uint8_t* bgra_pixels;
} MemeStudioImageBufferV2;

typedef struct MemeStudioCancelTokenV2 MemeStudioCancelTokenV2;

enum MemeStudioLogLevelV2 {
    MEMESTUDIO_LOG_DEBUG = 0,
    MEMESTUDIO_LOG_INFO  = 1,
    MEMESTUDIO_LOG_WARN  = 2,
    MEMESTUDIO_LOG_ERROR = 3
};

typedef struct MemeStudioHostV2 {
    uint32_t struct_size;
    uint32_t abi_version;
    void* host_ctx;

    void* (MEMESTUDIO_CALL *alloc)(void* host_ctx, size_t size);
    void  (MEMESTUDIO_CALL *free)(void* host_ctx, void* ptr);
    void  (MEMESTUDIO_CALL *free_bytes)(void* host_ctx, MemeStudioBytesV2* bytes);
    void  (MEMESTUDIO_CALL *log)(void* host_ctx, int32_t level, const char* plugin_id, const char* utf8_msg);

    int32_t (MEMESTUDIO_CALL *is_cancelled)(const MemeStudioCancelTokenV2* cancel);

    int32_t (MEMESTUDIO_CALL *get_data_dir)(void* host_ctx, const char* plugin_id, MemeStudioBytesV2* out_utf8_path);
    int32_t (MEMESTUDIO_CALL *protect_secret)(void* host_ctx, const char* plain_utf8, MemeStudioBytesV2* out_cipher_b64);
    int32_t (MEMESTUDIO_CALL *unprotect_secret)(void* host_ctx, const char* cipher_b64, MemeStudioBytesV2* out_plain_utf8);

    int32_t (MEMESTUDIO_CALL *http_send)(
        void* host_ctx,
        const char* req_spec_json,
        const uint8_t* body,
        size_t body_len,
        MemeStudioCancelTokenV2* cancel,
        void (MEMESTUDIO_CALL *on_sse_event)(void* user, const char* event_type, const char* event_data_utf8),
        void* user,
        uint32_t* out_status,
        int32_t* out_streamed,
        MemeStudioBytesV2* out_resp_body,
        MemeStudioBytesV2* out_err_utf8);

    int32_t (MEMESTUDIO_CALL *probe_media)(
        void* host_ctx,
        const char* path_utf8,
        MemeStudioBytesV2* out_meta_json,
        MemeStudioBytesV2* out_err_utf8);

    int32_t (MEMESTUDIO_CALL *decode_media)(
        void* host_ctx,
        const char* path_utf8,
        const uint8_t* raw_bytes,
        size_t raw_len,
        MemeStudioBytesV2* out_meta_json,
        MemeStudioImageBufferV2** out_frames,
        size_t* out_frame_count,
        MemeStudioBytesV2* out_err_utf8);

    void (MEMESTUDIO_CALL *free_images)(
        void* host_ctx,
        MemeStudioImageBufferV2* frames,
        size_t frame_count);

    int32_t (MEMESTUDIO_CALL *encode_media)(
        void* host_ctx,
        const char* path_utf8_or_null,
        const char* format_json,
        const MemeStudioImageBufferV2* frames,
        size_t frame_count,
        MemeStudioBytesV2* out_encoded_bytes_or_null,
        MemeStudioBytesV2* out_err_utf8);

    int32_t (MEMESTUDIO_CALL *get_workspace_state)(void* host_ctx, MemeStudioBytesV2* out_state_json);
    int32_t (MEMESTUDIO_CALL *set_session_endpoint_override)(
        void* host_ctx,
        const char* plugin_id,
        const char* endpoint_json_or_null);
    int32_t (MEMESTUDIO_CALL *present_output_file)(void* host_ctx, const char* path_utf8);
    int32_t (MEMESTUDIO_CALL *run_core_replace_job)(
        void* host_ctx,
        const char* job_json,
        MemeStudioCancelTokenV2* cancel,
        void (MEMESTUDIO_CALL *on_progress)(void* user, const char* progress_json),
        void* user,
        MemeStudioBytesV2* out_result_json,
        MemeStudioBytesV2* out_err_utf8);

    int32_t (MEMESTUDIO_CALL *open_panel)(
        void* host_ctx,
        const char* panel_key,
        void* owner_hwnd,
        int32_t width_dip,
        int32_t height_dip,
        const char* title_utf8,
        intptr_t (MEMESTUDIO_CALL *wnd_proc)(void* panel_ctx, void* hwnd, uint32_t msg, uintptr_t wp, intptr_t lp),
        void* panel_ctx,
        void (MEMESTUDIO_CALL *on_destroy)(void* panel_ctx));
} MemeStudioHostV2;

typedef struct MemeStudioPluginV2 {
    uint32_t struct_size;
    uint32_t abi_version;
    const char* id;

    int32_t (MEMESTUDIO_CALL *create)(const MemeStudioHostV2* host, void** out_instance, MemeStudioBytesV2* out_err_utf8);
    void    (MEMESTUDIO_CALL *destroy)(void* instance);

    int32_t (MEMESTUDIO_CALL *invoke_command)(
        void* instance,
        const char* command_id,
        const char* args_json,
        void* owner_hwnd,
        MemeStudioCancelTokenV2* cancel,
        MemeStudioBytesV2* out_result_json,
        MemeStudioBytesV2* out_err_utf8);

    int32_t (MEMESTUDIO_CALL *transform_prompt)(
        void* instance,
        const char* context_json,
        const char* prompt_utf8,
        MemeStudioBytesV2* out_prompt_utf8,
        MemeStudioBytesV2* out_err_utf8);

    int32_t (MEMESTUDIO_CALL *process_frames_batch)(
        void* instance,
        const char* stage,
        const char* context_json,
        MemeStudioImageBufferV2* frames,
        size_t frame_count,
        MemeStudioCancelTokenV2* cancel,
        MemeStudioBytesV2* out_err_utf8);

    int32_t (MEMESTUDIO_CALL *intercept_image_api)(
        void* instance,
        const char* op,
        const char* provider_json,
        const char* request_json,
        const MemeStudioBytesV2* uploads,
        size_t upload_count,
        MemeStudioCancelTokenV2* cancel,
        void (MEMESTUDIO_CALL *on_preview)(void* user, const uint8_t* png_bytes, size_t png_len, int32_t partial_idx),
        void* user,
        MemeStudioBytesV2* out_response_bytes,
        MemeStudioBytesV2* out_err_utf8);
} MemeStudioPluginV2;

typedef const MemeStudioPluginV2* (MEMESTUDIO_CALL *MemeStudioPluginEntryV2)(void);

#ifdef __cplusplus
}
#endif

#endif /* MEMESTUDIO_PLUGIN_H */
