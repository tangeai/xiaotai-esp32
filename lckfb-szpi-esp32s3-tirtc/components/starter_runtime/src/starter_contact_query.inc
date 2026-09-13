/* Included by the runtime owner. Queries never dial, stop AI, or block on HTTP.
 * Pending request, output and JSON view nodes live in PSRAM. cJSON's existing
 * contacts parser is reused; no global allocator hooks are changed. */
#define CONTACT_QUERY_MAX 16U
#define CONTACT_QUERY_TIMEOUT_MS 10000

typedef struct {
    uint32_t ticket;
    uint32_t session;
    uint32_t connection;
    int64_t started;
    char id_text[129];
    cJSON id;
    char target[65];
    char filter[8];
    cJSON rows[CONTACT_QUERY_MAX][6];
    char output[RUNTIME_TEXT_MAX];
} contact_query_t;

static EXT_RAM_BSS_ATTR contact_query_t *s_contact_query;
static EXT_RAM_BSS_ATTR uint32_t s_contact_query_ticket;

/* Borrowed cJSON views: AddItemToObjectCS and PrintPreallocated allocate
 * nothing. Never cJSON_Delete these stack/PSRAM nodes or their source strings. */
static void contact_query_reply(const cJSON *id, bool ok, const char *status,
                                const char *message, cJSON *contacts, unsigned count,
                                contact_query_t *query)
{
    if (!cJSON_IsString(id) && !cJSON_IsNumber(id)) {
        ESP_LOGW(TAG, "AI query: missing/invalid request id; no response");
        return;
    }
    char small_output[1536];
    char count_text[12];
    (void)snprintf(count_text, sizeof(count_text), "%u", count);
    cJSON root = {.type = cJSON_Object};
    cJSON result = {.type = cJSON_Object};
    cJSON error = {.type = cJSON_Object};
    int error_code = !strcmp(status, "invalid_params") ? -32602 :
                     !strcmp(status, "unsupported_action") ? -32601 : -32000;
    cJSON code = {.type = cJSON_Number, .valueint = error_code, .valuedouble = error_code};
    cJSON error_message = {.type = cJSON_String, .valuestring = (char *)message};
    cJSON fields[] = {
        {.type = cJSON_String, .valuestring = "2.0"},
        *id,
        {.type = ok ? cJSON_True : cJSON_False},
        {.type = cJSON_String, .valuestring = (char *)status},
        {.type = cJSON_String, .valuestring = (char *)message},
        {.type = cJSON_String, .valuestring = count_text},
        {.type = cJSON_Array},
    };
    fields[1].next = fields[1].prev = NULL;
    fields[1].string = NULL;
    (void)cJSON_AddItemToObjectCS(&root, "jsonrpc", &fields[0]);
    (void)cJSON_AddItemToObjectCS(&root, "id", &fields[1]);
    /* JSON-RPC failures use error, not a successful result with ok=false. */
    if (ok) {
        (void)cJSON_AddItemToObjectCS(&root, "result", &result);
    } else {
        (void)cJSON_AddItemToObjectCS(&root, "error", &error);
        (void)cJSON_AddItemToObjectCS(&error, "code", &code);
        (void)cJSON_AddItemToObjectCS(&error, "message", &error_message);
        (void)cJSON_AddItemToObjectCS(&error, "data", &result);
    }
    (void)cJSON_AddItemToObjectCS(&result, "ok", &fields[2]);
    (void)cJSON_AddItemToObjectCS(&result, "status", &fields[3]);
    (void)cJSON_AddItemToObjectCS(&result, "message", &fields[4]);
    /* The currently configured cloud plugin declares count as string. */
    (void)cJSON_AddItemToObjectCS(&result, "count", &fields[5]);
    (void)cJSON_AddItemToObjectCS(&result, "contacts", contacts ? contacts : &fields[6]);
    char *output = query ? query->output : small_output;
    size_t capacity = query ? sizeof(query->output) : sizeof(small_output);
    if (!cJSON_PrintPreallocated(&root, output, (int)capacity, false)) {
        ESP_LOGW(TAG, "AI query: response exceeds %u bytes", (unsigned)capacity);
        if (contacts != NULL)
            contact_query_reply(id, false, "response_too_large",
                                "查询结果过大，请指定联系人。", NULL, 0, query);
        return;
    }
    /* Like the legacy AI owner, correlate by JSON id and send raw 0x2100.
     * The incoming SDK sequence/response bits are not the JSON-RPC reply id. */
    int rc = starter_tirtc_send_command(AI_COMMAND, output, (uint32_t)strlen(output));
    ESP_LOGI(TAG, "AI query tx: q=%lu status=%s count=%u bytes=%u rc=%d",
             (unsigned long)(query ? query->ticket : 0), status, count,
             (unsigned)strlen(output), rc);
    if (rc < 0) ESP_LOGW(TAG, "AI query response send failed: %d", rc);
}

static void contact_query_clear(void)
{
    heap_caps_free(s_contact_query);
    s_contact_query = NULL;
}

static bool contact_query_current(uint32_t ticket)
{
    return s_contact_query != NULL && s_contact_query->ticket == ticket &&
           s_contact_query->session == s_session_generation &&
           s_contact_query->connection == s_connection_generation &&
           atomic_load(&s_public_state) == STARTER_RUNTIME_AI_ACTIVE;
}

static void contact_query_tick(void)
{
    if (s_contact_query == NULL) return;
    if (!contact_query_current(s_contact_query->ticket)) {
        ESP_LOGI(TAG, "AI query cancelled: session changed");
        contact_query_clear();
    } else if (now_ms() - s_contact_query->started >= CONTACT_QUERY_TIMEOUT_MS) {
        contact_query_reply(&s_contact_query->id, false, "timeout",
                            "联系人查询超时，请稍后重试。", NULL, 0, s_contact_query);
        contact_query_clear();
    }
}

static void contact_query_response(const char *body, void *user_data)
{
    runtime_event_t event = {
        .type = EVENT_CONTACT_QUERY_RESULT,
        .request_tag = (uint32_t)(uintptr_t)user_data,
    };
    size_t length = body ? strnlen(body, RUNTIME_TEXT_MAX + 1U) : 0;
    if (!body) event.error = ESP_FAIL;
    else if (length > RUNTIME_TEXT_MAX) event.error = ESP_ERR_INVALID_SIZE;
    else {
        event.text = heap_caps_malloc(length + 1U, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (event.text == NULL) event.error = ESP_ERR_NO_MEM;
        else {
            memcpy(event.text, body, length);
            event.text[length] = '\0';
            event.length = (uint32_t)length;
        }
    }
    if (!queue_event(&event)) {
        ESP_LOGE(TAG, "AI query HTTP delivery failed: q=%lu; deadline remains active",
                 (unsigned long)event.request_tag);
        release_event(&event);
    }
}

static void start_contact_query(const cJSON *id, const cJSON *params)
{
    contact_query_tick();
    const cJSON *payload = action_payload(params);
    const cJSON *target = cJSON_GetObjectItemCaseSensitive(payload, "target");
    const cJSON *filter = cJSON_GetObjectItemCaseSensitive(payload, "status_filter");
    bool valid_id = cJSON_IsNumber(id) ||
        (cJSON_IsString(id) && id->valuestring && strlen(id->valuestring) <= 128U);
    if (!valid_id || (target && (!cJSON_IsString(target) ||
        strlen(target->valuestring) > 64U)) || (filter && !cJSON_IsString(filter))) {
        contact_query_reply(id, false, "invalid_params", "查询参数无效。", NULL, 0, NULL);
        return;
    }
    const char *filter_text = filter ? filter->valuestring : "online";
    if (strcmp(filter_text, "online") && strcmp(filter_text, "offline") && strcmp(filter_text, "all")) {
        contact_query_reply(id, false, "invalid_params",
                            "status_filter 只能是 online、offline 或 all。", NULL, 0, NULL);
        return;
    }
    if (s_contact_query != NULL) {
        contact_query_reply(id, false, "busy", "已有联系人查询正在进行。", NULL, 0, NULL);
        return;
    }
    contact_query_t *query = heap_caps_calloc(1, sizeof(*query), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!query) {
        contact_query_reply(id, false, "no_memory", "联系人查询内存不足。", NULL, 0, NULL);
        return;
    }
    query->id = *id;
    query->id.next = query->id.prev = NULL;
    query->id.string = NULL;
    if (cJSON_IsString(id)) {
        (void)snprintf(query->id_text, sizeof(query->id_text), "%s", id->valuestring);
        query->id.valuestring = query->id_text;
    }
    (void)snprintf(query->target, sizeof(query->target), "%s", target ? target->valuestring : "");
    (void)snprintf(query->filter, sizeof(query->filter), "%s", filter_text);
    if (++s_contact_query_ticket == 0) ++s_contact_query_ticket;
    query->ticket = s_contact_query_ticket;
    query->session = s_session_generation;
    query->connection = s_connection_generation;
    query->started = now_ms();
    s_contact_query = query;
    ESP_LOGI(TAG, "AI query rx: q=%lu gen=%lu target_len=%u filter=%s psram=%u",
             (unsigned long)query->ticket, (unsigned long)query->session,
             (unsigned)strlen(query->target), query->filter, (unsigned)sizeof(*query));
    esp_err_t err = platform_client_request_timeout(PLATFORM_SERVICE_CALL,
        "/v1/call/device/contacts", NULL, 4000U, contact_query_response,
        (void *)(uintptr_t)query->ticket);
    if (err != ESP_OK) {
        contact_query_reply(&query->id, false, "unavailable",
                            "联系人服务暂不可用。", NULL, 0, query);
        contact_query_clear();
    }
}

static void complete_contact_query(const runtime_event_t *event, const cJSON *root)
{
    contact_query_t *query = s_contact_query;
    const cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");
    const cJSON *contacts = cJSON_GetObjectItemCaseSensitive(data, "contacts");
    ESP_LOGI(TAG, "AI query HTTP: q=%lu bytes=%lu err=%d elapsed=%lldms",
             (unsigned long)event->request_tag, (unsigned long)event->length,
             event->error, (long long)(now_ms() - query->started));
    const char *error = NULL;
    if (event->error == ESP_ERR_INVALID_SIZE) error = "response_too_large";
    else if (event->error || !response_ok(root) || !cJSON_IsArray(contacts)) error = "upstream_error";
    cJSON array = {.type = cJSON_Array};
    unsigned count = 0, assumed = 0;
    char normalized_target[65];
    bool has_target = normalize_contact_name(query->target, normalized_target, sizeof(normalized_target));
    bool target_by_id = false;
    const cJSON *item;
    if (!error && has_target) cJSON_ArrayForEach(item, contacts) {
        const cJSON *device = cJSON_GetObjectItemCaseSensitive(item, "device_id");
        if (cJSON_IsString(device) && !strcmp(query->target, device->valuestring)) {
            target_by_id = true;
            break;
        }
    }
    if (!error) cJSON_ArrayForEach(item, contacts) {
        const cJSON *type = cJSON_GetObjectItemCaseSensitive(item, "type");
        const cJSON *device = cJSON_GetObjectItemCaseSensitive(item, "device_id");
        const cJSON *remark = cJSON_GetObjectItemCaseSensitive(item, "remark");
        const cJSON *online = cJSON_GetObjectItemCaseSensitive(item, "online");
        if (!cJSON_IsString(type) || !cJSON_IsString(device) || !device->valuestring[0] ||
            (strcmp(type->valuestring, "device") && strcmp(type->valuestring, "voip"))) {
            error = "upstream_error";
            break;
        }
        bool wechat = strcmp(type->valuestring, "voip") == 0;
        const char *name = cJSON_IsString(remark) && remark->valuestring[0] ?
            remark->valuestring : (wechat ? "微信联系人" : device->valuestring);
        char normalized_name[65];
        if (has_target) {
            bool matches = target_by_id ? !strcmp(query->target, device->valuestring) :
                (normalize_contact_name(name, normalized_name, sizeof(normalized_name)) &&
                 !strcmp(normalized_target, normalized_name));
            if (!matches) continue;
        }
        bool known = !wechat && cJSON_IsBool(online);
        bool is_online = wechat || cJSON_IsTrue(online);
        if (!wechat && !known && (has_target || strcmp(query->filter, "all"))) {
            error = "status_unavailable";
            break;
        }
        /* A named target asks its status, even when the list default is online. */
        if (!has_target && strcmp(query->filter, "all") &&
            ((!wechat && !known) || (is_online != !strcmp(query->filter, "online")))) continue;
        if (count == CONTACT_QUERY_MAX) { error = "response_too_large"; break; }
        cJSON *row = query->rows[count];
        row[0] = (cJSON){.type = cJSON_Object};
        row[1] = (cJSON){.type = cJSON_String, .valuestring = (char *)name};
        row[2] = (cJSON){.type = cJSON_String, .valuestring = device->valuestring};
        row[3] = (cJSON){.type = !known && !wechat ? cJSON_NULL : (is_online ? cJSON_True : cJSON_False)};
        row[4] = (cJSON){.type = cJSON_String, .valuestring = type->valuestring};
        row[5] = (cJSON){.type = known ? cJSON_True : cJSON_False};
        (void)cJSON_AddItemToObjectCS(&row[0], "name", &row[1]);
        (void)cJSON_AddItemToObjectCS(&row[0], "device_id", &row[2]);
        (void)cJSON_AddItemToObjectCS(&row[0], "online", &row[3]);
        (void)cJSON_AddItemToObjectCS(&row[0], "type", &row[4]);
        (void)cJSON_AddItemToObjectCS(&row[0], "online_known", &row[5]);
        (void)cJSON_AddItemToArray(&array, &row[0]);
        ++count;
        assumed += wechat;
    }
    if (!error && has_target && count == 0) error = "not_found";
    if (!error && has_target && count > 1) error = "ambiguous";
    contact_query_reply(&query->id, error == NULL, error ? error : "ok",
        error ? "联系人查询失败，未使用过期状态。" : assumed ?
        "查询成功；微信联系人按约定视为可联系，非实时在线检测。" : "查询成功。",
        error ? NULL : &array, error ? 0 : count, query);
    contact_query_clear();
}
