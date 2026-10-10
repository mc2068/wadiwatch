"""MQTT adapter: subscribes to the gateway uplink topic and hands each payload to ingest.

It holds no logic of its own. Everything about a message is decided by the
ingest function it is given.
"""

import logging

log = logging.getLogger("wadiwatch.mqtt")

DEFAULT_PORT = 1883
UPLINK_TOPIC = "wadiwatch/up"
CLIENT_ID = "wadiwatch-backend"
# At least once: the gateway may repeat a message and ingest ignores the copy.
QOS = 1


class MqttUplink:
    def __init__(
        self, ingest, host, port=DEFAULT_PORT, topic=UPLINK_TOPIC, username=None, password=None
    ):
        self._ingest = ingest
        self._host = host
        self._port = port
        self._topic = topic
        self._username = username
        self._password = password
        self._client = None

    def start(self):
        import paho.mqtt.client as mqtt

        # A fixed client identifier with a persistent session lets the broker
        # keep messages for the backend while it is restarting.
        client = mqtt.Client(
            mqtt.CallbackAPIVersion.VERSION2, client_id=CLIENT_ID, clean_session=False
        )
        if self._username:
            client.username_pw_set(self._username, self._password)
        client.on_connect = self.on_connect
        client.on_message = self.on_message
        client.connect_async(self._host, self._port)
        client.loop_start()
        self._client = client
        log.info("MQTT uplink: %s:%s topic %s", self._host, self._port, self._topic)

    def stop(self):
        if self._client is not None:
            self._client.loop_stop()
            self._client.disconnect()
            self._client = None

    def on_connect(self, client, userdata, flags, reason_code, properties):
        # Subscribing here renews the subscription after every reconnect.
        if reason_code.is_failure:
            log.error("MQTT broker refused the connection: %s", reason_code)
            return
        client.subscribe(self._topic, qos=QOS)

    def on_message(self, client, userdata, message):
        result = self._ingest(message.payload)
        log.debug("MQTT message on %s: %s", message.topic, result.status)
