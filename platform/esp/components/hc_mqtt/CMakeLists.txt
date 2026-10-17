# Copyright (c) 2026 Bernhard Haumacher
#
# The MQTT client on the device (issue #18, doc/mqtt.md): hc::MqttClient on esp-mqtt (part of ESP-IDF; TLS for
# mqtts:// with the IDF certificate bundle), its events posted to the event loop through the LoopQueue, the
# MqttService that owns it with the core's hc::Mqtt, and the console command `mqtt`.

idf_component_register(
	SRCS "esp_mqtt_client.cpp" "mqtt_service.cpp"
	INCLUDE_DIRS "include"
	REQUIRES hc_core hc_platform hc_console
	PRIV_REQUIRES mqtt mbedtls freertos log)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
