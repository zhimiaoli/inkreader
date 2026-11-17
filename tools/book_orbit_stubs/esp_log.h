#pragma once
/* 空实现但要引用 tag，避免 -Werror=unused-variable。 */
/* No-op but must reference the tag to keep -Werror=unused-variable quiet. */
#define ESP_LOGI(tag, ...) ((void)(tag))
#define ESP_LOGW(tag, ...) ((void)(tag))
#define ESP_LOGE(tag, ...) ((void)(tag))
