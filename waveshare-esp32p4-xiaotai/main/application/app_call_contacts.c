#include "app.h"

#include <string.h>

#include "device_call.h"
#include "device_online.h"

static void app_call_contacts_append_by_presence(app_call_contacts_snapshot_t *snapshot,
						 const device_call_contacts_snapshot_t *cloud,
						 bool online)
{
	for (uint8_t index = 0;
	     index < cloud->count && snapshot->count < APP_CALL_CONTACT_MAX;
	     ++index) {
		const device_call_contact_t *source = &cloud->contacts[index];
		app_call_contact_t *target = &snapshot->contacts[snapshot->count];

		if (source->online != online ||
		    strlen(source->device_id) != APP_CALL_CONTACT_DEVICE_ID_LENGTH ||
		    strlen(source->device_id) >= sizeof(target->device_id)) {
			continue;
		}

		strlcpy(target->device_id, source->device_id, sizeof(target->device_id));
		strlcpy(target->remark, source->remark, sizeof(target->remark));
		target->online = source->online;
		target->deletable = source->source == DEVICE_CALL_CONTACT_SOURCE_MANUAL;
		++snapshot->count;
	}
}

static void app_call_contacts_copy_pending(app_call_contacts_snapshot_t *snapshot,
					   const device_call_contacts_snapshot_t *cloud)
{
	for (uint8_t index = 0;
	     index < cloud->pending_count && snapshot->pending_count < APP_CALL_CONTACT_MAX;
	     ++index) {
		const device_call_pending_contact_t *source = &cloud->pending[index];
		app_call_pending_contact_t *target = &snapshot->pending[snapshot->pending_count];

		if (strlen(source->peer_device_id) != APP_CALL_CONTACT_DEVICE_ID_LENGTH ||
		    strlen(source->peer_device_id) >= sizeof(target->device_id)) {
			continue;
		}

		strlcpy(target->device_id, source->peer_device_id, sizeof(target->device_id));
		strlcpy(target->created_at, source->created_at, sizeof(target->created_at));
		++snapshot->pending_count;
	}
}

esp_err_t app_add_call_contact(const char *device_id)
{
	if (device_id == NULL ||
	    strlen(device_id) != APP_CALL_CONTACT_DEVICE_ID_LENGTH) {
		return ESP_ERR_INVALID_ARG;
	}

	return device_call_request_contact_async(device_id);
}

esp_err_t app_respond_call_contact(const char *device_id, bool accept)
{
	if (device_id == NULL ||
	    strlen(device_id) != APP_CALL_CONTACT_DEVICE_ID_LENGTH) {
		return ESP_ERR_INVALID_ARG;
	}

	return device_call_respond_contact_async(device_id, accept);
}

esp_err_t app_update_call_contact_remark(const char *device_id, const char *remark)
{
	if (device_id == NULL ||
	    strlen(device_id) != APP_CALL_CONTACT_DEVICE_ID_LENGTH ||
	    remark == NULL || strlen(remark) >= APP_CALL_CONTACT_REMARK_MAX) {
		return ESP_ERR_INVALID_ARG;
	}

	return device_call_update_contact_remark_async(device_id, remark);
}

esp_err_t app_delete_call_contact(const char *device_id)
{
	if (device_id == NULL ||
	    strlen(device_id) != APP_CALL_CONTACT_DEVICE_ID_LENGTH) {
		return ESP_ERR_INVALID_ARG;
	}

	return device_call_delete_contact_async(device_id);
}

esp_err_t app_refresh_call_contacts(void)
{
	return device_call_refresh_contacts_async();
}

void app_get_call_contacts(app_call_contacts_snapshot_t *snapshot)
{
	device_call_contacts_snapshot_t cloud = {0};
	device_call_snapshot_t call = {0};

	if (snapshot == NULL) {
		return;
	}

	memset(snapshot, 0, sizeof(*snapshot));
	device_call_get_contacts_snapshot(&cloud);
	device_call_get_snapshot(&call);
	snapshot->ready = cloud.ready;
	snapshot->refreshing = cloud.refreshing;
	snapshot->last_error = cloud.last_error;

	/* Keep callable peers first when the cloud account also contains offline devices. */
	app_call_contacts_append_by_presence(snapshot, &cloud, true);
	app_call_contacts_append_by_presence(snapshot, &cloud, false);
	app_call_contacts_copy_pending(snapshot, &cloud);

	if (!cloud.ready && !cloud.refreshing && device_online_is_online() &&
	    (call.state == DEVICE_CALL_STATE_IDLE || call.state == DEVICE_CALL_STATE_ERROR)) {
		(void)device_call_refresh_contacts_async();
	}
}
