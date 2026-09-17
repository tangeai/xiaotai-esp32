/*
 * TiCloudStorage 公共 C 接口
 *
 * TiCloudStorage 接收开发者持续送入的编码音视频帧，并根据上传请求选择需要
 * 上传到云端的媒体时间范围。
 *
 * 所有时间戳统一使用 UTC 毫秒。
 */

#ifndef TICLOUDSTORAGE_H
#define TICLOUDSTORAGE_H

#include "basedef.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TICLOUDSTORAGE_API TG_PUBLIC


#define TICLOUDSTORAGE_VERSION_MAJOR 0
#define TICLOUDSTORAGE_VERSION_MINOR 3
#define TICLOUDSTORAGE_VERSION_PATCH 0
#define TICLOUDSTORAGE_VERSION_STRING "0.3.0"

/*
 * TiCloudStorageUploadRequestOptions.start_time_ms 使用的特殊值。
 *
 * 表示从 Service 环形队列按写入顺序当前保留的第一个视频关键帧开始。
 * 设备产生的有效媒体时间戳不得使用 Unix Epoch 的 0 值。
 */
#define TICLOUDSTORAGE_TIME_EARLIEST 0

/* start_time_ms 最多可以晚于当前 UTC 时间 10 秒。 */
#define TICLOUDSTORAGE_MAX_FUTURE_START_MS UINT64_C(10000)

/* 单个 Service 同时存在的逻辑上传请求上限。 */
#define TICLOUDSTORAGE_MAX_UPLOADS_PER_SERVICE 64

/* Service 未配置最大关键帧间隔时使用 5 秒。 */
#define TICLOUDSTORAGE_DEFAULT_MAX_KEY_FRAME_INTERVAL_MS 5000U

/* 最大关键帧间隔配置上限；对应的最长起始等待时间为 60 秒。 */
#define TICLOUDSTORAGE_MAX_KEY_FRAME_INTERVAL_MS 20000U

/* 起始关键帧等待时间的下限。 */
#define TICLOUDSTORAGE_MIN_KEY_FRAME_WAIT_MS UINT64_C(10000)

/*
 * 通用返回值。
 *
 * 返回 ID 的函数成功时返回正数，失败时返回以下负数错误码。
 * 其他函数成功时返回 TICLOUDSTORAGE_OK，失败时返回负数错误码。
 *
 * 数值按类别预留。调用方必须比较具体错误码，不得根据数值范围推断是否
 * 可以重试；已发布错误码的数值不会改变或复用。
 */
enum TiCloudStorageError {
    TICLOUDSTORAGE_OK = 0,

    /* 通用错误：-50001～-50099。 */
    TICLOUDSTORAGE_E_INVALID_ARGUMENT = -50001,
    TICLOUDSTORAGE_E_NOT_INITIALIZED = -50002,
    TICLOUDSTORAGE_E_ALREADY_INITIALIZED = -50003,
    TICLOUDSTORAGE_E_NO_MEMORY = -50004,
    TICLOUDSTORAGE_E_INTERNAL = -50005,
    /* 仅供当前实现骨架使用，正式实现的已支持接口不得返回。 */
    TICLOUDSTORAGE_E_NOT_IMPLEMENTED = -50099,

    /* Service 错误：-50100～-50199。 */
    TICLOUDSTORAGE_E_SERVICE_NOT_FOUND = -50100,
    TICLOUDSTORAGE_E_SERVICE_INVALID_STATE = -50101,
    TICLOUDSTORAGE_E_SERVICE_ID_EXHAUSTED = -50102,
    TICLOUDSTORAGE_E_SERVICES_ACTIVE = -50103,

    /* 媒体和队列错误：-50200～-50299。 */
    TICLOUDSTORAGE_E_UNSUPPORTED_MEDIA = -50200,
    TICLOUDSTORAGE_E_TIMESTAMP_OUT_OF_ORDER = -50201,
    TICLOUDSTORAGE_E_QUEUE_FULL = -50202,

    /* Upload 错误：-50300～-50399。 */
    TICLOUDSTORAGE_E_UPLOAD_NOT_FOUND = -50300,
    TICLOUDSTORAGE_E_UPLOAD_FINISHED = -50301,
    TICLOUDSTORAGE_E_UPLOAD_RANGE_INVALID = -50302,
    TICLOUDSTORAGE_E_TOO_MANY_UPLOADS = -50303,
    TICLOUDSTORAGE_E_UPLOAD_START_TIMEOUT = -50304,

    /* 网络和鉴权错误：-50400～-50499。 */
    TICLOUDSTORAGE_E_AUTH_FAILED = -50400,
    TICLOUDSTORAGE_E_NETWORK_UNAVAILABLE = -50401,
    TICLOUDSTORAGE_E_NETWORK_TIMEOUT = -50402,
    TICLOUDSTORAGE_E_RATE_LIMITED = -50403,
    TICLOUDSTORAGE_E_SERVER_UNAVAILABLE = -50404,
    TICLOUDSTORAGE_E_PROTOCOL_ERROR = -50405,

    /* 持久化缓存错误：-50500～-50599。 */
    TICLOUDSTORAGE_E_CACHE_FULL = -50500,
    TICLOUDSTORAGE_E_CACHE_CORRUPTED = -50501,
    TICLOUDSTORAGE_E_CACHE_IN_USE = -50502
};

/*
 * 上传结果。
 *
 * COMPLETE：
 *   选中的媒体范围已经连续、完整地上传到云端。
 *
 * PARTIAL：
 *   至少有一段录像可以播放，但成功范围中存在一个或多个缺口。
 *   最终结果只返回累计摘要，不列出缺口所在的具体时间片。
 *
 * FAILED：
 *   没有形成任何可播放的录像区间。
 */
enum TiCloudStorageUploadResultCode {
    TICLOUDSTORAGE_UPLOAD_COMPLETE = 0,
    TICLOUDSTORAGE_UPLOAD_PARTIAL = 1,
    TICLOUDSTORAGE_UPLOAD_FAILED = 2
};

/*
 * 媒体格式。
 *
 * TiCloudStorageFrameInfo.media 使用以下值之一。数值与现有 TiRTC 媒体编号保持兼容。
 */
enum TiCloudStorageMedia {
    TICLOUDSTORAGE_AUDIO_MIN = 1,
    TICLOUDSTORAGE_AUDIO_PCM = TICLOUDSTORAGE_AUDIO_MIN,
    TICLOUDSTORAGE_AUDIO_ALAW = 2,
    TICLOUDSTORAGE_AUDIO_AAC = 3,
    TICLOUDSTORAGE_AUDIO_OPUS = 4,
    TICLOUDSTORAGE_AUDIO_ULAW = 5,
    TICLOUDSTORAGE_AUDIO_G726 = 6,
    TICLOUDSTORAGE_AUDIO_MAX = 64,

    TICLOUDSTORAGE_VIDEO_MIN = 65,
    TICLOUDSTORAGE_VIDEO_JPEG = TICLOUDSTORAGE_VIDEO_MIN,
    TICLOUDSTORAGE_VIDEO_H264 = 66,
    TICLOUDSTORAGE_VIDEO_H265 = 67,
    TICLOUDSTORAGE_VIDEO_MAX = 127
};

/*
 * 判断媒体编号所属类别，与 TiRTC 的媒体编号范围和宏命名保持一致。
 *
 * 这些宏只判断编号是否落在音频或视频预留范围内，不表示当前 SDK 已经支持
 * 该具体媒体格式。传入参数应是变量或字段，不得使用自增、函数调用等带有
 * 副作用的表达式。
 */
#define TICLOUDSTORAGE_IS_AUDIO(media) \
    ((media) >= TICLOUDSTORAGE_AUDIO_MIN && (media) <= TICLOUDSTORAGE_AUDIO_MAX)

#define TICLOUDSTORAGE_IS_VIDEO(media) \
    ((media) >= TICLOUDSTORAGE_VIDEO_MIN && (media) <= TICLOUDSTORAGE_VIDEO_MAX)

/* 视频帧的 TiCloudStorageFrameInfo.flags。 */
enum TiCloudStorageVideoFrameFlag {
    TICLOUDSTORAGE_FRAME_FLAG_KEY_FRAME = 0x01
};

/* 音频帧的 TiCloudStorageFrameInfo.flags。 */
enum TiCloudStorageAudioSample {
    TICLOUDSTORAGE_AUDIO_SAMPLE_8K16B1C = 0,
    TICLOUDSTORAGE_AUDIO_SAMPLE_16K16B1C = 1,
    TICLOUDSTORAGE_AUDIO_SAMPLE_8K16B2C = 2,
    TICLOUDSTORAGE_AUDIO_SAMPLE_16K16B2C = 3
};

/* ------------------------------------------------------------------------- */
/* 第 1 步：配置回调并初始化进程级 SDK 资源                              */
/* ------------------------------------------------------------------------- */

enum TiCloudStorageLogLevel {
    /*
     * 使用 SDK 默认日志级别，当前为 TICLOUDSTORAGE_LOG_INFO。
     *
     * 该值为 0，因此使用 TICLOUDSTORAGE_OPTIONS_INITIALIZER 初始化后不设置
     * log_level 即可获得默认行为。
     */
    TICLOUDSTORAGE_LOG_DEFAULT = 0,
    TICLOUDSTORAGE_LOG_TRACE = 1,
    TICLOUDSTORAGE_LOG_DEBUG = 2,
    TICLOUDSTORAGE_LOG_INFO = 3,
    TICLOUDSTORAGE_LOG_WARN = 4,
    TICLOUDSTORAGE_LOG_ERROR = 5,
    TICLOUDSTORAGE_LOG_NONE = 6
};

struct TiCloudStorageOptions {
    /*
     * 本结构体的字节大小，必须设置为 sizeof(struct TiCloudStorageOptions)。
     *
     * SDK 只读取 struct_size 覆盖的字段。后续版本只能在结构体末尾追加字段：
     * 新版 SDK 对旧版较小结构体缺少的字段使用默认值；旧版 SDK 忽略新版
     * 较大结构体末尾无法识别的字段。
     */
    uint32_t struct_size;

    /*
     * TiCloudStorage 服务地址。
     *
     * 传 NULL 时使用 SDK 内置的公有云地址。
     */
    const char *endpoint;

    /*
     * 最低日志级别，使用 TiCloudStorageLogLevel。传 TICLOUDSTORAGE_LOG_DEFAULT 时使用
     * SDK 默认日志级别，当前为 TICLOUDSTORAGE_LOG_INFO。
     *
     * 传 TICLOUDSTORAGE_LOG_NONE 时关闭全部日志输出。
     */
    int log_level;

    /*
     * 接收 SDK 日志。传 NULL 时使用 SDK 默认日志输出。
     *
     * message 只在回调执行期间有效。回调内不得阻塞、发起网络请求或调用
     * 耗时的存储接口；需要异步处理时，应先复制 message。
     *
     * TiCloudStorageInit() 在返回前复制该函数指针。
     */
    void (*on_log)(int level, const char *message);
};

#define TICLOUDSTORAGE_OPTIONS_INITIALIZER \
    { (uint32_t)sizeof(struct TiCloudStorageOptions), 0, TICLOUDSTORAGE_LOG_DEFAULT, 0 }

/*
 * 初始化 TiCloudStorage。
 *
 * 同一时刻只能存在一次成功初始化。重复调用返回
 * TICLOUDSTORAGE_E_ALREADY_INITIALIZED；TiCloudStorageUninit() 成功后可以重新初始化。
 * options 可以为 NULL；此时使用内置服务地址、TICLOUDSTORAGE_LOG_INFO 和 SDK
 * 默认日志输出。
 *
 * options 非 NULL 时，struct_size 必须正确初始化。推荐使用
 * TICLOUDSTORAGE_OPTIONS_INITIALIZER 后再按需设置其他字段。
 *
 * TiCloudStorage 在函数返回前复制配置中的所有字符串和回调函数指针。
 */
TICLOUDSTORAGE_API int TiCloudStorageInit(const struct TiCloudStorageOptions *options);

/* ------------------------------------------------------------------------- */
/* 异步回调数据                                                             */
/* ------------------------------------------------------------------------- */

/*
 * 上传处理的一段连续媒体范围。
 *
 * 本结构体只描述范围，不表示上传成功或失败。具体结果由使用该结构体的
 * 接口或回调定义。用于物理上传回调时，范围属于融合录像文件，不对应某个
 * 单独的 channel_id。
 *
 * 时间口径为分片逻辑时间槽：[start_time_ms, end_time_ms) 恒为
 * [Tss, Tss+5000)，Tss 为视频帧驱动的固定 5 秒网格起点（见 ADR 0004）。
 * 与分片内首末帧的实际时间戳无关。
 */
struct TiCloudStorageUploadRange {
    /* 逻辑时间槽的开始时间（Tss），包含该时间点。 */
    uint64_t start_time_ms;

    /* 逻辑时间槽的结束时间（Tss+5000），不包含该时间点。 */
    uint64_t end_time_ms;

    /*
     * 成功回调时，是该物理录像文件经云端确认成功的实际上传字节数；
     * 失败回调时为 0。重试产生的重复网络流量不计入该值。
     */
    uint64_t size_bytes;
};

/*
 * 已经确定结束边界并完成处理的上传请求最终结果。
 *
 * 所有时间范围均描述融合录像文件的上传结果，不表示单个 Channel 的媒体
 * 完整性或上传结果。
 *
 * 本结构体只包含固定大小摘要，不返回物理上传时间片数组。需要定位具体
 * 时间片的调用方应注册 on_progress 并自行记录。
 *
 * message 和本结果结构体均由 SDK 管理，只在回调执行期间有效。
 */
struct TiCloudStorageUploadResult {
    /* SDK 为逻辑上传请求生成的 ID。 */
    int upload_id;

    /* TiCloudStorageUploadRequestOptions.start_time_ms 的请求值。 */
    uint64_t requested_start_time_ms;

    /*
     * 最终确定的请求结束边界。该值来自实际选中的第一个视频关键帧时间戳
     * 加 duration_ms，或 TiCloudStorageUploadSetEnd() 传入的 end_time_ms；两者
     * 同时存在时取较早值。
     */
    uint64_t requested_end_time_ms;

    /* 最早上传成功逻辑槽的开始时间（Tss）；没有成功区间时为 0。 */
    uint64_t start_time_ms;

    /* 最晚上传成功逻辑槽的右开结束时间（Tss+5000）；没有成功区间时为 0。 */
    uint64_t end_time_ms;

    /*
     * 所有上传成功逻辑槽的累计时长（每槽 5000ms，裁剪到请求边界），
     * 不包含中间缺口；没有成功区间时为 0。
     *
     * 存在缺口时：
     *
     *     net_duration_ms < end_time_ms - start_time_ms
     */
    uint64_t net_duration_ms;

    /* 上传请求的最终结果，使用 TiCloudStorageUploadResultCode。 */
    int result;

    /* TICLOUDSTORAGE_OK 或描述失败原因的负数错误码。 */
    int error;

    /* 可选的诊断信息，可能为 NULL。 */
    const char *message;

    /*
     * 全部成功区间的融合媒体数据总量（经云端确认的实际上传字节数）。
     * 重试产生的重复网络流量不计入该值。
     */
    uint64_t size_bytes;
};

/* ------------------------------------------------------------------------- */
/* 第 2 步：创建并配置 Service                                              */
/* ------------------------------------------------------------------------- */

/*
 * Service 创建配置。
 *
 * 所有回调均由 SDK 的回调分发线程执行。回调内不得阻塞或调用
 * TiCloudStorageServiceStop()、TiCloudStorageServiceDestroy()、TiCloudStorageUninit() 等等待
 * 回调线程退出的接口。Token 回调可以调用 TiCloudStorageServiceUpdateToken()；
 * SDK 执行回调时不持有会导致该调用重入死锁的内部锁。
 *
 * TiCloudStorageServiceCreate() 在返回前复制全部配置字段和回调函数指针。
 */
struct TiCloudStorageServiceOptions {
    /*
     * 本结构体的字节大小，必须设置为
     * sizeof(struct TiCloudStorageServiceOptions)。
     *
     * 兼容规则与 TiCloudStorageOptions.struct_size 相同。
     */
    uint32_t struct_size;

    /*
     * Token 即将过期时触发。是否启用及提前量由 token_expire_warning_sec
     * 决定。service_id 标识触发回调的 Service，可直接传给
     * TiCloudStorageServiceUpdateToken()。expires_at_ms 是 Token 的 UTC 毫秒
     * 过期时间。user_data 来自 TiCloudStorageServiceOptions.user_data。
     * 不需要时传 NULL。
     */
    void (*on_token_will_expire)(
        int service_id,
        uint64_t expires_at_ms,
        void *user_data
    );

    /*
     * Token 已经过期时触发。SDK 按本地时钟（JWT exp）判定到期会触发一次；
     * 此外每次平台调用以 Token 无效/过期（AuthFailure.Token*）拒绝请求时
     * 都会触发——不做去重计数，上层可依赖每次拒绝都收到一次通知。
     * expires_at_ms 是 Token 的 UTC 毫秒过期时间。Token 过期只暂停云端
     * 传输，不停止写队列、不清空环形队列，也不关闭上传任务。service_id
     * 标识触发回调的 Service，可直接传给 TiCloudStorageServiceUpdateToken()。
     * user_data 来自 TiCloudStorageServiceOptions.user_data。不需要时传 NULL。
     */
    void (*on_token_expired)(
        int service_id,
        uint64_t expires_at_ms,
        void *user_data
    );

    /*
     * SDK 调用上述 Token 回调时原样传回。SDK 不访问也不释放该指针；
     * 调用方必须保证它在 TiCloudStorageServiceDestroy() 返回前始终有效。
     *
     * service_id 由 SDK 通过回调参数明确提供；本字段只保存开发者自己的
     * Service 级业务上下文。
     */
    void *user_data;

    /*
     * Service 鉴权使用的设备密钥。
     *
     * 必填。TiCloudStorage 在函数返回前复制该字符串。SDK 不得将其写入日志。
     */
    const char *device_secret_key;

    /*
     * 当前 Service 的最近帧环形队列容量，单位字节。
     *
     * 队列保存该 Service 最近送入的媒体帧，使上传任务可以选择仍未被覆盖的
     * 历史帧。传 0 时使用 SDK 默认值。
     */
    uint32_t buffer_size_bytes;

    /*
     * 当前 Service 待上传数据的可选持久化目录，目录必须可写。
     *
     * 传 NULL 或空字符串时关闭持久化缓存。
     *
     * 缓存在本目录内使用 endpoint + device_id 作为可跨进程重启恢复的
     * 稳定命名空间，service_id 只标识当前运行期所有者。同一命名空间同一
     * 时刻只能由一个存活的 Service 独占；已被占用时
     * TiCloudStorageServiceCreate() 返回 TICLOUDSTORAGE_E_CACHE_IN_USE。
     */
    const char *cache_dir;

    /*
     * 当前 Service 的持久化缓存空间上限，单位字节。
     *
     * 启用 cache_dir 时传 0 表示使用 SDK 默认值。
     */
    uint64_t cache_size_bytes;

    /*
     * Token 到期前多少秒触发即将过期回调。
     *
     * 传 0 时不触发预警回调。
     */
    uint32_t token_expire_warning_sec;

    /*
     * 当前 Service 所有视频 Channel 中允许的最大关键帧间隔，单位毫秒。
     *
     * 传 0 时使用 TICLOUDSTORAGE_DEFAULT_MAX_KEY_FRAME_INTERVAL_MS。允许范围为
     * 1～TICLOUDSTORAGE_MAX_KEY_FRAME_INTERVAL_MS。SDK 等待请求起始关键帧的
     * 超时时间为：
     *
     *     max(3 * max_key_frame_interval_ms,
     *         TICLOUDSTORAGE_MIN_KEY_FRAME_WAIT_MS)
     *
     * 因此默认等待 15 秒，最大等待 60 秒。调用方应根据该 Service 中间隔
     * 最长的视频 Channel 配置；所有 Channel 都必须满足该上限。
     */
    uint32_t max_key_frame_interval_ms;

    /*
     * 上传队列最大长度（待传切片数）。
     *
     * 队列满时丢弃最旧的待传切片并计入对应请求的缺口。
     * 传 0 时使用 SDK 默认值 2；允许范围为 1～6（SDK 内部上限）。
     * 超出范围时 TiCloudStorageServiceCreate() 返回
     * TICLOUDSTORAGE_E_INVALID_ARGUMENT。
     */
    uint32_t upload_queue_max_slices;

    /*
     * 上传强制使用 http（明文，不带 TLS），仅对本 Service 的上传生效。
     *
     * 传非 0 开启：上传 URL 一律使用 http://，凭证 endpoint 中已有的
     * https:// 也会被改写为 http://。传 0 使用默认行为。
     * 默认行为：SSL 编译（CONFIG_SSL_SUPPORT 非 0）为 https；
     * 无 SSL 编译（CONFIG_SSL_SUPPORT==0）恒为 http，本字段无效果。
     */
    uint32_t upload_force_http;
};

#define TICLOUDSTORAGE_SERVICE_OPTIONS_INITIALIZER \
    { \
        (uint32_t)sizeof(struct TiCloudStorageServiceOptions), \
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 \
    }

/*
 * 创建一个独立 Service。
 *
 * Service 是一条独立媒体时间线，也是媒体输入和上传任务的资源隔离边界。
 * 每个 Service 内部由两个相互独立、通过媒体帧引用关联的核心对象组成：
 *
 *   1. 容量有限的最近帧环形队列
 *      - TiCloudStorageQueueWriteFrame() 向队列写入音频或视频帧；
 *      - TiCloudStorageQueueGetInfo() 查询队列当前状态；
 *      - 队列按写帧接口的实际受理顺序保存最近编码帧，不对不同 Channel
 *        的帧做全局时间戳重排；
 *      - 容量不足时覆盖最早写入且未进入上传处理链路的帧；
 *      - 没有可覆盖空间时，写入返回 TICLOUDSTORAGE_E_QUEUE_FULL；
 *      - 队列本身不决定哪些帧需要上传。
 *
 *   2. 异步上传任务管理器
 *      - TiCloudStorageUploadRequest() 创建一个上传请求；
 *      - TiCloudStorageUploadSetEnd() 设置或提前收敛请求的结束边界；
 *      - 任务根据 start_time_ms，从环形队列及之后写入的帧中，选择任意
 *        视频 Channel 中第一个时间戳满足下界的关键帧作为请求实际起点；
 *      - 其他视频 Channel 各自在此后遇到自己的第一个关键帧时加入录像，
 *        加入前的非关键视频帧不写入录像，离线 Channel 不阻塞请求；
 *      - 上传任务选择并引用位于目标时间范围内、且符合各 Channel 起点规则
 *        的音视频帧；
 *      - 网络传输、重试、持久化缓存和结果回调均在异步任务中执行；
 *      - 未指定持续时长的请求保持开启，直到调用 TiCloudStorageUploadSetEnd()；
 *      - 多个重叠任务共享同一份帧数据，不重复复制或上传媒体内容。
 *
 * 因此，送帧接口只操作环形队列，Upload 接口只操作异步上传任务。
 * 上传请求覆盖该 Service 接收的全部媒体通道，不选择 channel_id。
 * 当前版本将同一物理时间片内的全部 Channel 融合到一个录像文件并只上传
 * 一次；物理上传回调只报告该文件的时间范围和结果，不区分摄像头。
 *
 * 本接口只分配和初始化本地资源，不启动异步处理，也不接受媒体帧或上传请求。
 * 创建成功后，可以先调用 TiCloudStorageServiceUpdateToken() 设置上传授权，再调用
 * TiCloudStorageServiceStart() 启动 Service。
 *
 * options->struct_size 必须正确初始化。推荐使用
 * TICLOUDSTORAGE_SERVICE_OPTIONS_INITIALIZER 后再按需设置其他字段。
 *
 * 成功返回正数 service_id，失败返回负数错误码。
 * service_id 在当前进程内不会分配给其他 Service；Service 销毁后也不复用。
 * 分配值到达 INT_MAX 后，后续创建返回
 * TICLOUDSTORAGE_E_SERVICE_ID_EXHAUSTED，不发生整数回绕。
 */
TICLOUDSTORAGE_API int TiCloudStorageServiceCreate(
    const char *device_id,
    const struct TiCloudStorageServiceOptions *options
);

/*
 * 更新当前 Service 的 Token。
 *
 * TiCloudStorage 将 Token 作为不透明的授权字符串处理，并在函数返回前完成复制。
 * Token 只决定上传权限和云端保存策略，不选择“事件”或“持续”录像模式。
 *
 * 本接口可以在 Service 创建后、启动前调用，也可以在运行期间更新 Token。
 * 换入新 Token 后，on_token_will_expire / on_token_expired 按新 Token 重新
 * 武装。重复下发与当前相同的 Token：
 * - 当前未到期时视为空操作，不重新武装也不重复触发回调——业务侧续签
 *   拿到同一 Token 时不会形成续签热循环；
 * - 当前已到期时重新武装，on_token_expired 可再次触发，续签失败重投
 *   同一 Token 时回调不断流。
 */
TICLOUDSTORAGE_API int TiCloudStorageServiceUpdateToken(
    int service_id,
    const char *access_token
);

/* ------------------------------------------------------------------------- */
/* 第 3 步：启动 Service                                                     */
/* ------------------------------------------------------------------------- */

/*
 * 启动已经创建并完成配置的 Service。
 *
 * 启动成功后，Service 才开始接受 TiCloudStorageQueueWriteFrame()、
 * TiCloudStorageUploadRequest() 和 TiCloudStorageUploadSetEnd() 调用，并启用 Token
 * 到期通知、异步上传、重试、持久化恢复和结果回调。
 *
 * 没有有效 Token 不影响启动。媒体帧和上传请求仍可进入队列，云端传输等待
 * TiCloudStorageServiceUpdateToken() 提供有效 Token 后自动继续。
 *
 * Service 只能启动一次。已经启动或已经停止的 Service 再次调用本接口返回
 * TICLOUDSTORAGE_E_SERVICE_INVALID_STATE。
 */
TICLOUDSTORAGE_API int TiCloudStorageServiceStart(int service_id);

/* ------------------------------------------------------------------------- */
/* 第 4 步：提交上传请求并设置请求结束边界                                */
/* ------------------------------------------------------------------------- */

/*
 * 单个逻辑上传请求的可选配置和异步回调。
 *
 * TiCloudStorageUploadRequest() 在返回前复制所有字段和回调函数指针，不保留本结构体
 * 地址。on_progress 和 on_result 共享本请求的 user_data。
 *
 * 两个回调均由 SDK 的回调分发线程执行。回调内不得阻塞或调用
 * TiCloudStorageServiceStop()、TiCloudStorageServiceDestroy()、TiCloudStorageUninit() 等等待
 * 回调线程退出的接口。
 */
struct TiCloudStorageUploadRequestOptions {
    /*
     * 本结构体的字节大小，必须设置为
     * sizeof(struct TiCloudStorageUploadRequestOptions)。
     *
     * 兼容规则与 TiCloudStorageOptions.struct_size 相同。
     */
    uint32_t struct_size;

    /*
     * 搜索 Service 环形队列时使用的时间下界。融合录像只维护一套包含全部
     * 视频 Channel 的关键帧索引，索引顺序与环形队列写入顺序一致。请求
     * 创建时，SDK 选择索引中第一个时间戳大于等于 start_time_ms 的关键帧；
     * 当前没有候选时，后续第一个实际受理的合法视频关键帧启动请求。
     * SDK 不按 Channel 分别选择起点，也不按时间戳重排关键帧索引。
     *
     * 传 TICLOUDSTORAGE_TIME_EARLIEST 表示选择队列按写入顺序当前保留的第一个
     * 视频关键帧。设备产生的有效媒体时间戳不得使用 Unix Epoch 的 0 值。
     */
    uint64_t start_time_ms;

    /*
     * 传 0 时请求保持开启，直到调用 TiCloudStorageUploadSetEnd() 或 Service 停止。
     * 传正数时，从实际选中的第一个视频关键帧开始计算自动结束边界。
     */
    uint64_t duration_ms;

    /*
     * 本请求引用的某个物理上传时间片进入最终状态时触发；同一时间片对本
     * 请求只触发一次。成功时 error 为 TICLOUDSTORAGE_OK；失败时仅在错误不可重试
     * 或 SDK 重试耗尽后触发，error 为最终负数错误码。临时失败和正在重试
     * 不触发。
     *
     * service_id 标识本请求所属 Service。range 表示融合录像文件的连续
     * 媒体范围，不包含 channel_id。物理时间片可能被多个逻辑请求共享，
     * SDK 仍只上传一次，但会分别使用各请求配置的回调和 user_data 通知。
     *
     * user_data 来自本 TiCloudStorageUploadRequestOptions.user_data，属于请求级
     * 上下文。range 只在回调执行期间有效。回调必须快速返回；不需要时
     * 传 NULL。
     */
    void (*on_progress)(
        int service_id,
        const struct TiCloudStorageUploadRange *range,
        int error,
        void *user_data
    );

    /*
     * 本请求关闭并进入最终状态时触发，一个请求只触发一次。service_id
     * 标识本请求所属 Service；user_data 来自本
     * TiCloudStorageUploadRequestOptions.user_data，属于请求级上下文。
     *
     * 无限时长请求在调用 TiCloudStorageUploadSetEnd() 前不会正常结束。Service
     * Stop 会中止请求且不补发本回调。不需要时传 NULL。
     */
    void (*on_result)(
        int service_id,
        const struct TiCloudStorageUploadResult *result,
        void *user_data
    );

    /*
     * SDK 调用本请求的 on_progress 和 on_result 时原样传回。
     * SDK 不访问也不释放该指针。同步受理失败时不保留该指针，也不触发请求
     * 回调；成功受理后的回调不会在 TiCloudStorageUploadRequest() 返回前触发。
     *
     * 正常完成时必须保持有效到 on_result 返回；未配置 on_result 时，调用方
     * 必须通过自己的请求生命周期保证有效。被 Service Stop 中止时必须保持
     * 有效到 Stop 返回。
     */
    void *user_data;
};

#define TICLOUDSTORAGE_UPLOAD_REQUEST_OPTIONS_INITIALIZER \
    { \
        (uint32_t)sizeof(struct TiCloudStorageUploadRequestOptions), \
        0, 0, 0, 0, 0 \
    }

/*
 * 提交一个覆盖整个 Service 的逻辑上传请求。
 *
 * options 可以为 NULL；此时 start_time_ms、duration_ms 均为 0，不注册请求
 * 回调，也不设置 user_data。options 非 NULL 时，struct_size 必须正确
 * 初始化，推荐使用 TICLOUDSTORAGE_UPLOAD_REQUEST_OPTIONS_INITIALIZER。
 *
 * 本接口只表示 SDK 已接受请求并分配 upload_id，不表示已经找到实际上传
 * 起点，不表示已经开始网络传输，也不表示任何媒体数据已经上传成功。
 *
 * start_time_ms 最多可以晚于调用时的当前 UTC 时间
 * TICLOUDSTORAGE_MAX_FUTURE_START_MS；超过该范围时同步返回
 * TICLOUDSTORAGE_E_UPLOAD_RANGE_INVALID。每个 Service 同时最多存在
 * TICLOUDSTORAGE_MAX_UPLOADS_PER_SERVICE 个未结束的逻辑请求，等待起始关键帧的
 * 请求也计入该上限；已结束的请求释放槽位供新请求复用（若有在途上报条目
 * 仍引用该槽位，则暂不复用）。无可复用槽位时同步返回
 * TICLOUDSTORAGE_E_TOO_MANY_UPLOADS。
 *
 * 起始等待从请求受理时间和 start_time_ms 中较晚的时刻开始，使用单调时钟
 * 计时。等待超时由 TiCloudStorageServiceOptions.max_key_frame_interval_ms 确定；
 * 超时前仍未找到合法关键帧时，请求以 FAILED 结束，并通过 on_result 返回
 * TICLOUDSTORAGE_E_UPLOAD_START_TIMEOUT。
 *
 * 任意视频 Channel 的第一个合法关键帧都可以启动请求。其他视频 Channel
 * 不阻塞请求，各自在遇到自己的第一个合法关键帧后加入录像；加入前的非关键
 * 视频帧不写入录像。分片以视频帧计时：每个分片的首帧必须是视频帧，
 * 首视频帧之前的音频被丢弃；分片内音频与视频按时间序写入。音频不能
 * 单独启动请求。
 * 某一路 Channel 长期离线不影响其他 Channel；所有 Channel 都没有视频
 * 关键帧时，请求在等待超时后失败。
 *
 * 实际上传起点可能晚于 start_time_ms。duration_ms > 0 时，自动结束边界为：
 *
 *     实际选中的第一个视频关键帧时间戳 + duration_ms
 *
 * 返回正数只表示请求已创建；SDK 随后异步查找关键帧、等待媒体时间线到达
 * 结束边界，并根据 Token、网络和重试状态执行实际上传。
 *
 * Service 必须已经成功启动，否则返回 TICLOUDSTORAGE_E_SERVICE_INVALID_STATE。
 * upload_id 由 SDK 生成，在 Service 生命周期内唯一，只用于
 * TiCloudStorageUploadSetEnd() 等请求控制，不承载业务含义。开发者不需要使用
 * upload_id 关联回调和业务上下文。
 */
TICLOUDSTORAGE_API int TiCloudStorageUploadRequest(
    int service_id,
    const struct TiCloudStorageUploadRequestOptions *options
);

/*
 * 请求设置或提前收敛逻辑上传请求的绝对结束时间。
 *
 * 本接口只更新请求的结束边界，不表示媒体时间线已经到达该位置，不表示
 * SDK 已经停止实际网络传输，也不表示上传请求已经完成。
 *
 * end_time_ms 必填，是 UTC 毫秒形式的右开边界。时间戳大于等于
 * end_time_ms 的帧不属于本次上传请求。
 * Service 必须处于运行状态，否则返回 TICLOUDSTORAGE_E_SERVICE_INVALID_STATE。
 *
 * 如果请求已经配置 duration_ms，则取两个结束边界中较早的一个：
 *
 *   min(实际选中的第一个视频关键帧时间戳 + duration_ms, end_time_ms)
 *
 * 重复设置相同边界时幂等成功；设置更早边界时继续收缩请求范围；不得将
 * 已经确定的结束边界向后扩大。
 *
 * end_time_ms 可以晚于当前已经收到的最新帧。此时请求继续保持活动，SDK
 * 继续检查后续帧，直到 Service 媒体时间线实际到达该边界。
 *
 * 请求仍在等待起始关键帧时也可以调用本接口。SDK 继续从 start_time_ms
 * 向后查找关键帧；如果媒体时间线到达最终结束边界时仍没有找到边界之前的
 * 合法关键帧，请求以失败结束。
 *
 * 本接口不会取消请求。最终结束边界之前的数据继续异步处理，时间戳大于
 * 等于最终结束边界的数据不再属于该逻辑请求。即使边界之后的物理数据已经
 * 因其他请求进入上传链路，也不会计入本请求结果。
 *
 * 返回成功只表示新的结束边界已被接受。实际停止选帧和上传完成均为异步
 * 过程，最终执行情况只通过对应请求的 on_result 回调返回。
 *
 * upload_id 不存在、不属于该 Service，或者请求已经进入最终状态时返回
 * 负数错误码，不修改任何请求状态，也不重复触发最终回调。
 */
TICLOUDSTORAGE_API int TiCloudStorageUploadSetEnd(
    int service_id,
    int upload_id,
    uint64_t end_time_ms
);

/* ------------------------------------------------------------------------- */
/* 第 5 步：查询或写入 Service 的媒体帧环形队列                           */
/* ------------------------------------------------------------------------- */

/*
 * Service 媒体帧环形队列的状态快照。
 *
 * 队列为空时，所有帧数量和时间戳字段均为 0。
 * 队列不为空但没有视频关键帧时，关键帧时间字段为 0。
 */
struct TiCloudStorageQueueInfo {
    /* 队列总容量，包括帧数据和 SDK 内部帧头占用的空间。 */
    uint64_t capacity_bytes;

    /* 当前已经使用的队列空间，包括帧数据和 SDK 内部帧头。 */
    uint64_t used_bytes;

    /*
     * used_bytes 中已经进入上传处理链路、尚未释放且暂时不能覆盖的空间。
     * 包括等待上传、正在传输和等待重试的数据，不仅指正在网络发送的数据。
     * 该值较高表示上传处理速度落后于写队列速度，可能很快产生队列压力。
     */
    uint64_t inflight_bytes;

    /* 当前队列中保存的音视频帧总数。 */
    uint64_t frame_count;

    /* 当前队列最早写入帧的时间戳，即物理队首帧携带的时间戳。 */
    uint64_t oldest_time_ms;

    /* 当前队列最新写入帧的时间戳，即物理队尾帧携带的时间戳。 */
    uint64_t latest_time_ms;

    /*
     * 当前全局关键帧索引中第一个保留关键帧的时间戳；JPEG 每帧均计入。
     */
    uint64_t oldest_key_frame_time_ms;

    /*
     * 当前全局关键帧索引中最后一个保留关键帧的时间戳；JPEG 每帧均计入。
     */
    uint64_t latest_key_frame_time_ms;
};

/*
 * 查询 Service 环形队列的当前状态。
 *
 * 本接口返回调用时刻的一致性快照，可以与写队列和上传任务并发调用。
 * SDK 在函数返回前写完 info，返回后调用方可以直接读取和保存该结构体。
 * 已创建、运行中或已停止的 Service 均可查询，销毁后 service_id 失效。
 */
TICLOUDSTORAGE_API int TiCloudStorageQueueGetInfo(
    int service_id,
    struct TiCloudStorageQueueInfo *info
);

struct TiCloudStorageFrameInfo {
    /*
     * 设备在当前 Service 内的媒体通道标识，取值范围为 0～255。
     *
     * SDK 将每个 channel_id 视为相互独立的媒体通道，并按照一个通道最多
     * 包含一路视频和一路音频处理。同一采集通道（通常为一个摄像头及其
     * 配套麦克风）的音频帧和视频帧应使用相同的 channel_id；不同采集通道
     * 应使用不同的 channel_id。单通道设备通常固定使用 0。
     *
     * channel_id 由调用方分配，在 Service 生命周期内必须保持稳定。对于
     * 主码流和子码流、多传感器组合等更灵活的场景，调用方应为不同的音视频
     * 组合分配不同的 channel_id，并在下载端维护这些 ID 与业务通道之间的
     * 映射；SDK 不解释不同 channel_id 之间的业务关系。
     *
     * 上传任务始终覆盖 Service 接收的全部媒体通道。
     */
    uint8_t channel_id;

    /* TICLOUDSTORAGE_AUDIO_* 或 TICLOUDSTORAGE_VIDEO_* 中的一个值。 */
    uint8_t media;

    /*
     * 视频：TICLOUDSTORAGE_FRAME_FLAG_KEY_FRAME 或 0。JPEG 每帧都按关键帧处理，
     * 不依赖该标志。
     * 音频：TICLOUDSTORAGE_AUDIO_SAMPLE_* 中的一个值。
     */
    uint8_t flags;

    /* 预留字段，必须填 0。 */
    uint8_t reserved;

    /*
     * UTC 毫秒采集时间戳，必须大于 0。
     *
     * 0 保留给 TICLOUDSTORAGE_TIME_EARLIEST 及空队列查询结果，不能作为媒体帧
     * 时间戳写入。
     */
    uint64_t timestamp_ms;

    /* 帧数据长度，单位字节。 */
    uint32_t length;
};

/*
 * 向 Service 环形队列写入一帧完整的编码音频或视频。
 *
 * Service 在函数返回前复制 frame_info 和 frame。返回成功只表示环形队列
 * 已经接收该帧，不表示该帧已经上传。本接口不执行或等待任何网络 I/O。
 *
 * Service 必须处于运行状态，否则返回 TICLOUDSTORAGE_E_SERVICE_INVALID_STATE。
 *
 * 本接口线程安全，支持多个生产者线程向同一 Service 并发写帧。SDK 在
 * Service 内部串行确定写入受理顺序，并按该顺序追加到环形队列；SDK
 * 不对不同 Channel 的帧做全局时间戳重排。调用方仍须保证同一 channel_id、
 * 同一媒体类型的时间戳单调不减。
 *
 * SDK 根据 frame_info->media 判断音频或视频。每个 channel_id 内的音频和
 * 视频时间戳分别独立保持单调不减。
 */
TICLOUDSTORAGE_API int TiCloudStorageQueueWriteFrame(
    int service_id,
    const struct TiCloudStorageFrameInfo *frame_info,
    const void *frame
);

/* ------------------------------------------------------------------------- */
/* 第 6 步：停止并销毁 Service，然后释放进程级资源                        */
/* ------------------------------------------------------------------------- */

/*
 * 停止 Service。
 *
 * 调用前必须先停止所有送帧线程。本接口不再接受新的上传任务，不等待
 * 已经选中的媒体数据上传完成，也不排空环形队列或持久化缓存。
 *
 * 阻塞时间：已发出的 OSS PUT 与平台 HTTP 请求不会被打断，Stop 会等待
 * 在途请求自然结束（单次 PUT 超时 8~16 秒、重试一次，平台请求 10 秒，
 * 最坏约 35 秒）；正在执行的业务回调同样会运行到返回——若回调内部
 * 阻塞（Flash 写入/同步网络/等待锁），Stop 无法返回。设备关机/看门狗
 * 场景请把 Stop 与业务自身的中止机制放在同一线程模型下考虑。
 *
 * 被中止的上传请求不会触发 on_result，尚未分发的上传进度、Token
 * 和结果回调也会被丢弃。已经进入回调函数的调用会在本接口返回前结束；
 * 这仅用于保证回调和 user_data 的生命周期，不表示 SDK 正在等待上传完成。
 *
 * 如果业务需要等待上传完成或接收每个上传请求的最终结果，应在调用本接口
 * 前由上层跟踪 upload_id，并等待相应的 on_result 全部返回。SDK
 * 不在 Stop 过程中代替上层执行排空。
 *
 * 成功返回后 Service 进入已停止状态，不再触发任何 Service 回调，但本地
 * 资源仍然存在。错误返回时 Service 保持原状态，回调和 user_data 的生命
 * 周期也不发生变化。已经停止的 Service 不能再次启动。
 */
TICLOUDSTORAGE_API int TiCloudStorageServiceStop(int service_id);

/*
 * 销毁 Service 并释放其本地资源。
 *
 * 只能销毁尚未启动或已经停止的 Service；正在运行时返回
 * TICLOUDSTORAGE_E_SERVICE_INVALID_STATE。成功返回后 service_id 失效，SDK 不再
 * 访问 user_data。错误返回时 service_id 和 user_data 仍然有效。SDK 不会
 * 把已成功销毁的 ID 分配给后续创建的 Service；调用方仍应停止使用并自行
 * 清零保存该 ID 的变量。
 */
TICLOUDSTORAGE_API int TiCloudStorageServiceDestroy(int service_id);

/*
 * 释放进程级资源。
 *
 * 调用前必须销毁全部 Service。仍存在 Service 时返回
 * TICLOUDSTORAGE_E_SERVICES_ACTIVE，不执行部分清理；尚未初始化或已经成功反初始化
 * 时返回 TICLOUDSTORAGE_E_NOT_INITIALIZED。
 *
 * 成功返回 TICLOUDSTORAGE_OK。成功返回后不再触发任何回调，可以再次调用
 * TiCloudStorageInit()；进程级 service_id 分配器不会重置。
 */
TICLOUDSTORAGE_API int TiCloudStorageUninit(void);

/* ------------------------------------------------------------------------- */
/* 工具接口                                                                  */
/* ------------------------------------------------------------------------- */

/*
 * 获取 SDK 版本。
 *
 * 返回 TICLOUDSTORAGE_VERSION_STRING 对应的只读 NUL 结尾字符串。字符串由 SDK
 * 管理，调用方不得修改或释放。
 */
TICLOUDSTORAGE_API const char *TiCloudStorageGetVersion(void);

/*
 * 获取当前 SDK 二进制的详细构建信息。
 *
 * 返回以 NUL 结尾的只读字符串，包含版本、Git 提交、构建时间、目标平台、
 * 构建类型和编译器等诊断信息。字符串由 SDK 管理，在进程生命周期内有效，
 * 调用方不得修改或释放。
 *
 * 构建信息的字段和展示格式可能随版本扩展，不应作为稳定协议解析。需要判断
 * SDK 版本时，应使用 TiCloudStorageGetVersion()。
 */
TICLOUDSTORAGE_API const char *TiCloudStorageGetBuildInfo(void);

/*
 * 获取错误码的诊断文本。
 *
 * 返回只读 NUL 结尾字符串；未知错误码返回统一的 unknown 文本。字符串只
 * 用于日志和诊断，调用方不得修改、释放、解析或据此进行程序判断。
 */
TICLOUDSTORAGE_API const char *TiCloudStorageGetErrorString(int error);

#ifdef __cplusplus
}
#endif

#endif /* TICLOUDSTORAGE_H */
