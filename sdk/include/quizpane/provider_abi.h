#ifndef QUIZPANE_PROVIDER_ABI_H
#define QUIZPANE_PROVIDER_ABI_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#  if defined(QUIZPANE_PROVIDER_BUILD)
#    define QP_PROVIDER_EXPORT __declspec(dllexport)
#  else
#    define QP_PROVIDER_EXPORT __declspec(dllimport)
#  endif
#else
#  define QP_PROVIDER_EXPORT __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 为什么这里不用 C++ 虚函数接口？
 *
 * C++ ABI 会随编译器、标准库和编译选项变化，MSVC/Clang/GCC 之间尤其不能直接
 * 互通。这里采用 C ABI + UTF-8 JSON，相当于桌面进程内的一套“小型 HTTP/RPC
 * 协议”：函数指针是 endpoint，JSON 是请求/响应 DTO。这样 Host 与 Provider
 * 可以独立编译，只要 ABI 版本一致即可组合。
 */
#define QP_PROVIDER_ABI_V1 1u

typedef struct qp_provider_handle qp_provider_handle;

typedef enum qp_log_level {
    QP_LOG_DEBUG = 0,
    QP_LOG_INFO = 1,
    QP_LOG_WARNING = 2,
    QP_LOG_ERROR = 3
} qp_log_level;

typedef void (*qp_response_callback)(void* user_data, const char* response_json,
                                     size_t response_size);

/* Host 在创建 Provider 时注入的能力表，类似 Spring Bean 的依赖注入。
 * struct_size 允许以后只在结构末尾追加字段，同时兼容旧 Provider。
 * secure_* 由 Host 实现，Provider 因而不需要直接依赖 Keychain 等平台 API。 */
typedef struct qp_host_api_v1 {
    uint32_t abi_version;
    size_t struct_size;
    void* host_context;
    void (*log)(void* host_context, qp_log_level level, const char* message,
                size_t message_size);
    int (*secure_read)(void* host_context, const char* key, size_t key_size,
                       uint8_t* output, size_t* inout_output_size);
    int (*secure_write)(void* host_context, const char* key, size_t key_size,
                        const uint8_t* value, size_t value_size);
    int (*secure_delete)(void* host_context, const char* key, size_t key_size);
} qp_host_api_v1;

typedef uint32_t (*qp_provider_abi_version_fn)(void);
typedef const char* (*qp_provider_descriptor_json_fn)(void);
typedef int (*qp_provider_create_fn)(const qp_host_api_v1* host,
                                     qp_provider_handle** out_handle);
typedef int (*qp_provider_request_fn)(qp_provider_handle* handle,
                                      const char* request_json,
                                      size_t request_size,
                                      qp_response_callback callback,
                                      void* user_data);
typedef int (*qp_provider_cancel_fn)(qp_provider_handle* handle,
                                     const char* request_id,
                                     size_t request_id_size);
typedef void (*qp_provider_destroy_fn)(qp_provider_handle* handle);

/* 每个动态库必须导出以下六个符号。create/destroy 管理一个 Provider 实例；
 * request 异步返回 JSON；cancel 按请求 ID 取消尚未结束的网络操作。 */
QP_PROVIDER_EXPORT uint32_t qp_provider_abi_version(void);
QP_PROVIDER_EXPORT const char* qp_provider_descriptor_json(void);
QP_PROVIDER_EXPORT int qp_provider_create(const qp_host_api_v1* host,
                                          qp_provider_handle** out_handle);
QP_PROVIDER_EXPORT int qp_provider_request(qp_provider_handle* handle,
                                           const char* request_json,
                                           size_t request_size,
                                           qp_response_callback callback,
                                           void* user_data);
QP_PROVIDER_EXPORT int qp_provider_cancel(qp_provider_handle* handle,
                                          const char* request_id,
                                          size_t request_id_size);
QP_PROVIDER_EXPORT void qp_provider_destroy(qp_provider_handle* handle);

#ifdef __cplusplus
}
#endif

#endif
