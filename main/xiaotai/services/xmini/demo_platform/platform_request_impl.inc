// SPDX-License-Identifier: MIT
// Compiled in platform.c's queue/lock context; host tests use the same admission code.
esp_err_t platform_request_epoch(platform_operation_t operation, uint32_t gen,
                                 const char *body, unsigned *accepted_epoch)
{
    if (!platform_is_online() || !s_requests)
        return ESP_ERR_INVALID_STATE;
    const platform_route_t *route = platform_route(operation);
    if (!route) return ESP_ERR_INVALID_ARG;
    if (body && strlen(body) >= sizeof(((request_t *)0)->body))
        return ESP_ERR_INVALID_SIZE;
    if (route->method == HTTP_METHOD_GET) {
        if (body && body[0]) return ESP_ERR_INVALID_ARG;
    } else {
        cJSON *root = body ? cJSON_ParseWithOpts(body, NULL, true) : NULL;
        bool valid = cJSON_IsObject(root);
        cJSON_Delete(root);
        if (!valid) return ESP_ERR_INVALID_ARG;
    }
    request_t r = {.operation = operation, .generation = gen, .epoch = atomic_load(&s_epoch)};
    if (body)
        strcpy(r.body, body);
    portENTER_CRITICAL(&s_request_lock);
    bool blocked = s_response_blocked || s_http_gate.held;
    bool accepted = !blocked && xQueueSend(s_requests, &r, 0) == pdTRUE;
    if (accepted) ++s_http_gate.pending;
    portEXIT_CRITICAL(&s_request_lock);
    if (accepted && accepted_epoch) *accepted_epoch = r.epoch;
    return accepted ? ESP_OK : blocked ? ESP_ERR_INVALID_STATE : ESP_ERR_NO_MEM;
}

esp_err_t platform_request(platform_operation_t operation, uint32_t gen, const char *body)
{
    return platform_request_epoch(operation, gen, body, NULL);
}
